# Statera Ble Proxy
## Overview
Statera is an android application that functions as a bluetooth low energy proxy between a cyd_nightscout hardware device and nightscout web apis. it fetches requested data, compacts the json payloads to minimize bandwidth, and transmits the data back to the hardware.

## Core Components
- Mainactivity: requests necessary system permissions, launches the background service, and terminates.- Bleproxyservice: operates as a persistent foreground service. it manages bluetooth scanning, connection states, network requests, data parsing, and chunked bluetooth transmissions.

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
