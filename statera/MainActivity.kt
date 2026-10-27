package com.xyr0za.statera

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    // register permission launcher to handle user responses
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        // start service if all requested permissions are granted
        if (permissions.values.all { it }) {
            startProxyService()
        }
    }

    // initialize activity and request necessary permissions
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // determine required permissions based on android version
        val requiredPermissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        }

        // prompt user for permissions
        permissionLauncher.launch(requiredPermissions)
    }

    // launch foreground service and close activity
    private fun startProxyService() {
        val intent = Intent(this, BleProxyService::class.java)
        startForegroundService(intent)
        finish()
    }
}