# Statera Scout BLE Display

## Description
An ESP32-based Arduino sketch to display continuous glucose monitoring (CGM) data from Nightscout on a TFT display. It functions via Bluetooth Low Energy (BLE) payload transmission.

## Hardware Requirements
1.  ESP32 microcontroller with BLE capability.
2.  TFT display with an XPT2046 touch controller.
3.  Hardware LED pins defined for status indication.

## Software Dependencies
1.  TFT_eSPI library.
2.  ArduinoJson (version 7.x).
3.  ESP32 BLE Arduino libraries (BLEDevice, BLEServer, BLEUtils, BLE2902).

## Configuration
1.  Modify `NS_URL` to match the target Nightscout instance.
2.  Modify `API_SECRET` to the corresponding SHA1 hashing secret.
3.  Adjust `USE_MMOL` to specify standard (mg/dL) or UK metric (mmol/L) parameters.
4.  Configure `thrUrgentLow`, `thrLow`, `thrHigh`, and `thrUrgentHigh` to align with alerting requirements.
5.  Set `SCREEN_ROTATION` based on hardware mounting orientation.

## Usage
1.  Compile and flash to the ESP32.
2.  Pair with a compatible BLE source device.
3.  Tap the screen via the XPT2046 touch interface to cycle through the time graph ranges (1h, 3h, 6h).

<img width="1305" height="1740" alt="image" src="https://github.com/user-attachments/assets/8d639f54-c305-42d3-9414-2507d0f9ac83" />
Moving past the shoddy DIY work from me - we see that the code functions as expected within the CYD. 

# Statera Ble Proxy
## Overview
Statera is an android application that functions as a bluetooth low energy proxy between a cyd_nightscout hardware device and nightscout web apis. it fetches requested data, compacts the json payloads to minimize bandwidth, and transmits the data back to the hardware.

## Core Components
- Mainactivity: requests necessary system permissions, launches the background service, and terminates.
- Bleproxyservice: operates as a persistent foreground service. it manages bluetooth scanning, connection states, network requests, data parsing, and chunked bluetooth transmissions.

## Bluetooth Specifications
- target device name: cyd_nightscout
- service uuid: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
- rx characteristic: 6e400002-b5a3-f393-e0a9-e50e24dcca9e
- tx characteristic: 6e400003-b5a3-f393-e0a9-e50e24dcca9e
- target mtu: 185 bytes

## Supported Nightscout Endpoints
- /api/v1/entries (extracts glucose values, dates, and directions)
- /api/v1/status.json (extracts server time and glucose thresholds)
- /api/v1/treatments (extracts recent bolus insulin data)
- /api/v2/properties (extracts trimmed active insulin and active carbohydrates)

## System requirements
- android 12 (api 31) or higher requires bluetooth_scan and bluetooth_connect permissions.
- older android versions require access_fine_location permission for bluetooth scanning.
