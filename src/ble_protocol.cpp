#include "ble_protocol.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_mac.h>

#include "command_queue.h"
#include "config.h"
#include "protocol.h"
#include "valve_controller.h"

namespace {

constexpr const char* kServiceUuid = "7f0a0001-7b6b-4b5f-9d3e-3c7b9f100001";
constexpr const char* kRxUuid = "7f0a0002-7b6b-4b5f-9d3e-3c7b9f100001";
constexpr const char* kTxUuid = "7f0a0003-7b6b-4b5f-9d3e-3c7b9f100001";

BLEServer* g_server = nullptr;
BLEAdvertising* g_advertising = nullptr;
BLECharacteristic* g_txCharacteristic = nullptr;
SemaphoreHandle_t g_bleTxMutex = nullptr;
String g_deviceName;
uint32_t g_lastAdvertisingStart = 0;
bool g_initialized = false;

void setConnectionState(bool connected) {
  if (!opStateLock()) {
    return;
  }
  g_opState.bleConectado = connected;
  if (!connected) {
    g_opState.autenticado = false;
    g_opState.sessionId = "";
    g_opState.currentCmdId = "";
    if (g_opState.state != RUNNING) {
      g_opState.state = IDLE;
    }
  }
  opStateUnlock();
}

void enqueueIncomingLine(const String& line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    return;
  }

  Serial.printf("RX: %s\n", trimmed.c_str());
  if (!cmdQueue_enqueue(trimmed)) {
    bleProtocol_send("ERROR:QUEUE_FULL");
  }
}

String buildDeviceName() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char name[20] = {0};
  snprintf(name, sizeof(name), "%s%02X%02X", BLE_DEVICE_PREFIX, mac[4], mac[5]);
  return String(name);
}

class ServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer* server) override {
    (void)server;
    setConnectionState(true);
    Serial.println("DEVICE CONNECTED");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    setConnectionState(false);
    Serial.println("DEVICE DISCONNECTED");
    valveController_abortFromBleDisconnect();
    bleProtocol_startAdvertising();
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    if (value.empty()) {
      return;
    }

    String payload(value.c_str());
    payload.replace('\r', '\n');

    int start = 0;
    while (start < payload.length()) {
      int end = payload.indexOf('\n', start);
      if (end < 0) {
        end = payload.length();
      }
      enqueueIncomingLine(payload.substring(start, end));
      start = end + 1;
    }
  }
};

}  // namespace

void bleProtocol_init() {
  if (g_initialized) {
    return;
  }

  Serial.println("BLE INIT");

  g_bleTxMutex = xSemaphoreCreateMutex();
  configASSERT(g_bleTxMutex != nullptr);

  g_deviceName = buildDeviceName();

  BLEDevice::init(g_deviceName.c_str());
#if BLE_ENABLE_BONDING
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
#endif

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  BLEService* service = g_server->createService(kServiceUuid);

  g_txCharacteristic = service->createCharacteristic(
      kTxUuid,
      BLECharacteristic::PROPERTY_NOTIFY);
  g_txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* rxCharacteristic = service->createCharacteristic(
      kRxUuid,
      BLECharacteristic::PROPERTY_WRITE);
  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  g_advertising = BLEDevice::getAdvertising();
  g_advertising->addServiceUUID(kServiceUuid);
  g_advertising->setScanResponse(true);

  bleProtocol_startAdvertising();
  g_initialized = true;
}

void bleProtocol_send(const String& payload) {
  if (!g_txCharacteristic || payload.isEmpty()) {
    return;
  }

  if (!bleProtocol_isConnected()) {
    return;
  }

  if (xSemaphoreTake(g_bleTxMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  Serial.printf("TX: %s\n", payload.c_str());
  g_txCharacteristic->setValue(payload.c_str());
  g_txCharacteristic->notify();
  xSemaphoreGive(g_bleTxMutex);
}

void bleProtocol_startAdvertising() {
  if (!g_advertising) {
    return;
  }

  if (!opStateLock()) {
    return;
  }

  const bool connected = g_opState.bleConectado;
  opStateUnlock();
  if (connected) {
    return;
  }

  g_advertising->start();
  g_lastAdvertisingStart = millis();
}

void bleProtocol_restartAdvertisingIfNeeded() {
  if (!g_advertising) {
    return;
  }

  if (!opStateLock()) {
    return;
  }

  const bool connected = g_opState.bleConectado;
  opStateUnlock();
  if (connected) {
    return;
  }

  if ((millis() - g_lastAdvertisingStart) >= BLE_WATCHDOG_RESTART_MS) {
    g_advertising->start();
    g_lastAdvertisingStart = millis();
  }
}

String bleProtocol_getDeviceName() {
  return g_deviceName;
}

bool bleProtocol_isConnected() {
  bool connected = false;
  if (opStateLock()) {
    connected = g_opState.bleConectado;
    opStateUnlock();
  }
  return connected;
}

void taskBLE(void* param) {
  (void)param;
  bleProtocol_init();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
