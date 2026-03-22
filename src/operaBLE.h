#include "config.h"
#if !defined(_OPERA_BLE_) && defined(USAR_ESP32_UART_BLE)
    #define _OPERA_BLE_

    #include <BLEDevice.h>
    #include <BLEServer.h>
    #include <BLEUtils.h>
    #include <BLE2902.h>
    #include <BLESecurity.h>
    #include "esp_mac.h"          // esp_read_mac() para nome dinâmico CHOPP_XXXX
    #include "esp_gap_ble_api.h"  // esp_ble_gap_update_conn_params() para fix status=8

    #include "operacional.h"

    // ─────────────────────────────────────────────────────────────────────────
    // UUIDs do Nordic UART Service (NUS)
    // Compatível com nRF Toolbox, Serial Bluetooth Terminal e demais apps BLE UART
    // ─────────────────────────────────────────────────────────────────────────
    #define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    #define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Android → ESP32
    #define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // ESP32 → Android

    void setupBLE();
    void enviaBLE(String msg);

#endif
