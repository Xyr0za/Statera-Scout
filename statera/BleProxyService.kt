package com.xyr0za.statera

import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.ParcelUuid
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.UUID

@SuppressLint("MissingPermission")
class BleProxyService : Service() {

    // scope for background network operations
    private val serviceScope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // bluetooth connection state variables
    @Volatile private var bluetoothGatt: BluetoothGatt? = null
    @Volatile private var rxCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var mtu = 23
    @Volatile private var scanning = false
    @Volatile private var connecting = false

    // buffers for incoming requests from device to phone
    private val rxLock = Any()
    private val rxBuffer = StringBuilder()

    // synchronizes outgoing data from phone to device
    private val sendMutex = Mutex()
    private val writeAcks = Channel<Int>(Channel.CONFLATED)

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val RX_UUID: UUID      = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E") // phone writes here
        val TX_UUID: UUID      = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E") // phone gets notifications
        val CCCD_UUID: UUID    = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val CHANNEL_ID   = "STATERA_BLE_PROXY_CHANNEL"
        const val NOTIF_ID     = 1
        const val REQUESTED_MTU = 185 // matches firmware device mtu
    }

    // initialize foreground service and begin scanning
    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        val notification = createNotification("Scanning for CYD_Nightscout...")
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0
        ServiceCompat.startForeground(this, NOTIF_ID, notification, type)
        startBleScan()
    }

    // keep service running in the background
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    // clean up resources when service is destroyed
    override fun onDestroy() {
        stopScanSafely()
        serviceScope.cancel()
        bluetoothGatt?.let {
            it.disconnect()
            it.close()
        }
        bluetoothGatt = null
        super.onDestroy()
    }

    // binding is not used for this service
    override fun onBind(intent: Intent?): IBinder? = null

    // start bluetooth low energy scan for the target device
    private fun startBleScan() {
        if (scanning || connecting) return
        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            updateNotification("Bluetooth is off")
            retryScanLater(5000)
            return
        }
        val scanner = adapter.bluetoothLeScanner ?: run {
            retryScanLater(5000)
            return
        }

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanning = true
        updateNotification("Scanning for CYD_Nightscout...")
        scanner.startScan(listOf(filter), settings, scanCallback)
    }

    // safely stop the active bluetooth scan
    private fun stopScanSafely() {
        if (!scanning) return
        scanning = false
        try {
            val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            manager.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
    }

    // delay execution and attempt to scan again
    private fun retryScanLater(delayMs: Long) {
        serviceScope.launch {
            delay(delayMs)
            startBleScan()
        }
    }

    // handles scan results and initiates connection
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (connecting) return
            connecting = true
            stopScanSafely()
            updateNotification("Connecting to ${result.device.name ?: "CYD"}...")
            bluetoothGatt = result.device.connectGatt(
                this@BleProxyService, false, gattCallback, BluetoothDevice.TRANSPORT_LE
            )
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            updateNotification("Scan failed ($errorCode), retrying...")
            retryScanLater(5000)
        }
    }

    // manages bluetooth connection states and characteristics
    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                bluetoothGatt = gatt
                updateNotification("Connected, negotiating...")
                if (!gatt.requestMtu(REQUESTED_MTU)) gatt.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                gatt.close()
                bluetoothGatt = null
                rxCharacteristic = null
                mtu = 23
                synchronized(rxLock) { rxBuffer.setLength(0) }
                connecting = false
                updateNotification("Disconnected. Awaiting CYD...")
                retryScanLater(1000)
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) this@BleProxyService.mtu = mtu
            gatt.discoverServices()
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(SERVICE_UUID)
            val txChar = service?.getCharacteristic(TX_UUID)
            val rxChar = service?.getCharacteristic(RX_UUID)
            if (status != BluetoothGatt.GATT_SUCCESS || txChar == null || rxChar == null) {
                updateNotification("CYD service not found")
                gatt.disconnect()
                return
            }
            rxCharacteristic = rxChar

            gatt.setCharacteristicNotification(txChar, true)
            val descriptor = txChar.getDescriptor(CCCD_UUID)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                gatt.writeDescriptor(descriptor)
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (descriptor.uuid == CCCD_UUID) {
                updateNotification("Proxy ready (MTU $mtu)")
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            writeAcks.trySend(status)
        }

        // process data received from the device on api 33 and above
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleChunk(value)
        }

        // process data received from the device on older apis
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                @Suppress("DEPRECATION")
                handleChunk(characteristic.value ?: return)
            }
        }
    }

    // append bytes to buffer and process complete lines
    private fun handleChunk(bytes: ByteArray) {
        val lines = mutableListOf<String>()
        synchronized(rxLock) {
            rxBuffer.append(String(bytes, Charsets.UTF_8))
            if (rxBuffer.length > 4096) rxBuffer.setLength(0) // prevent buffer overflow
            while (true) {
                val idx = rxBuffer.indexOf("\n")
                if (idx < 0) break
                val line = rxBuffer.substring(0, idx).trim()
                rxBuffer.delete(0, idx + 1)
                if (line.isNotEmpty()) lines.add(line)
            }
        }
        lines.forEach { handleRequest(it) }
    }

    // parse device request and execute network call
    private fun handleRequest(line: String) {
        val parts = line.split("|", limit = 3)
        if (parts.size < 2 || parts[0] != "GET") return

        val urlString = parts[1]
        val secretHash = parts.getOrNull(2)

        serviceScope.launch {
            val response = try {
                fetchNightscoutData(urlString, secretHash)
            } catch (e: Exception) {
                val msg = (e.message ?: e.javaClass.simpleName)
                    .replace("|", " ").replace("\n", " ").take(30)
                "ERR|$msg"
            }
            sendToCyd(response + "\n")
        }
    }

    // perform http get request to nightscout api
    private fun fetchNightscoutData(urlString: String, apiSecretHash: String?): String {
        val secret = apiSecretHash?.takeIf { it.isNotBlank() }
            ?: Uri.parse(urlString).getQueryParameter("secret")

        val conn = (URL(urlString).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            if (!secret.isNullOrBlank()) setRequestProperty("API-SECRET", secret)
            setRequestProperty("Accept", "application/json")
            connectTimeout = 10000
            readTimeout = 15000
        }
        try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${conn.responseCode}")
            }
            val raw = conn.inputStream.bufferedReader().use { it.readText() }.trim()
            val path = URL(urlString).path
            return when {
                path.contains("/entries")     -> compactEntries(raw)
                path.endsWith("/status.json") -> compactStatus(raw)
                path.contains("/treatments")  -> compactTreatments(raw)
                path.contains("/properties")  -> compactProperties(raw)
                else -> raw.replace("\r", " ").replace("\n", " ")
            }
        } finally {
            conn.disconnect()
        }
    }

    // helper function to extract double from json
    private fun JSONObject.num(key: String): Double? {
        if (!has(key) || isNull(key)) return null
        val d = optDouble(key)
        return if (d.isNaN()) null else d
    }

    // helper function to extract string from json
    private fun JSONObject.str(key: String): String? =
        if (!has(key) || isNull(key)) null else optString(key).takeIf { it.isNotEmpty() }

    // extract glucose entry data from api response
    private fun compactEntries(raw: String): String {
        val src = JSONArray(raw)
        if (src.length() == 0) throw IOException("No entries")
        val out = JSONArray()
        for (i in 0 until src.length()) {
            val e = src.optJSONObject(i) ?: continue
            val sgv = e.optInt("sgv", 0)
            val date = e.optLong("date", e.optLong("mills", 0L))
            if (sgv <= 0 || date <= 0L) continue
            out.put(JSONObject().apply {
                put("sgv", sgv)
                put("date", date)
                put("direction", e.str("direction") ?: "")
            })
        }
        if (out.length() == 0) throw IOException("No valid SGV")
        return out.toString()
    }

    // extract server time and threshold data from api response
    private fun compactStatus(raw: String): String {
        val src = JSONObject(raw)
        val out = JSONObject()
        out.put("serverTimeEpoch", src.optLong("serverTimeEpoch", System.currentTimeMillis()))
        val thr = src.optJSONObject("settings")?.optJSONObject("thresholds")
        if (thr != null) {
            val t = JSONObject()
            for (k in listOf("bgHigh", "bgTargetTop", "bgTargetBottom", "bgLow")) {
                thr.num(k)?.let { t.put(k, it.toInt()) }
            }
            out.put("settings", JSONObject().put("thresholds", t))
        }
        return out.toString()
    }

    // extract recent bolus treatment data from api response
    private fun compactTreatments(raw: String): String {
        val src = JSONArray(raw)
        val out = JSONArray()
        for (i in 0 until src.length()) {
            val t = src.optJSONObject(i) ?: continue
            val units = t.optDouble("insulin", 0.0)
            if (units.isNaN() || units <= 0.0) continue
            val type = t.optString("eventType", "")
            if (type.contains("basal", ignoreCase = true)) continue
            var ms = t.optLong("mills", 0L)
            if (ms <= 0L) {
                val iso = t.optString("created_at", "")
                ms = try {
                    java.time.OffsetDateTime.parse(iso).toInstant().toEpochMilli()
                } catch (_: Exception) { 0L }
            }
            if (ms <= 0L) continue
            out.put(JSONObject().apply { put("insulin", units); put("date", ms) })
            break
        }
        return out.toString()
    }

    // format age object for properties json
    private fun ageObj(a: JSONObject): JSONObject = JSONObject().apply {
        put("found", a.optBoolean("found", false))
        a.num("age")?.let { put("age", it.toInt()) }
    }

    // extract trimmed properties data from api response
    private fun compactProperties(raw: String): String {
        val src = JSONObject(raw)
        val out = JSONObject()

        src.optJSONObject("iob")?.num("iob")?.let { out.put("iob", JSONObject().put("iob", it)) }
        src.optJSONObject("cob")?.num("cob")?.let { out.put("cob", JSONObject().put("cob", it)) }

        src.optJSONObject("sage")?.let { s ->
            val so = JSONObject()
            for (k in s.keys()) {
                val a = s.optJSONObject(k) ?: continue
                so.put(k, ageObj(a))
            }
            out.put("sage", so)
        }
        src.optJSONObject("cage")?.let { out.put("cage", ageObj(it)) }

        return out.toString()
    }

    // split payload and transmit to device over bluetooth
    private suspend fun sendToCyd(payload: String) = sendMutex.withLock {
        val gatt = bluetoothGatt ?: return@withLock
        val rxChar = rxCharacteristic ?: return@withLock
        val bytes = payload.toByteArray(Charsets.UTF_8)
        val chunkSize = (mtu - 3).coerceAtLeast(20)

        var offset = 0
        while (offset < bytes.size) {
            val end = minOf(offset + chunkSize, bytes.size)
            val chunk = bytes.copyOfRange(offset, end)

            writeAcks.tryReceive()
            if (!writeChunk(gatt, rxChar, chunk)) return@withLock

            val status = withTimeoutOrNull(3000) { writeAcks.receive() }
            if (status != BluetoothGatt.GATT_SUCCESS) return@withLock
            offset = end
        }
    }

    // execute write operation to bluetooth characteristic
    private fun writeChunk(
        gatt: BluetoothGatt,
        rxChar: BluetoothGattCharacteristic,
        chunk: ByteArray
    ): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(
                rxChar, chunk, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            rxChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            rxChar.value = chunk
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(rxChar)
        }
    }

    // construct notification channel for foreground service
    private fun createNotificationChannel() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Statera BLE Proxy", NotificationManager.IMPORTANCE_LOW)
        )
    }

    // construct ongoing notification object
    private fun createNotification(text: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Nightscout Proxy")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()

    // publish updated notification text to the system
    private fun updateNotification(text: String) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIF_ID, createNotification(text))
    }
}