#include "ble_protocol.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_gatts_api.h>
#include <esp_gap_ble_api.h>
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
uint32_t g_lastTxMs = 0;
uint32_t g_advBackoffMs = BLE_ADV_BACKOFF_START_MS;
uint32_t g_nextAdvertiseMs = 0;
bool g_advPending = false;
bool g_resumeSessionOnConnect = false;

void setConnectionState(bool connected) {
  if (!opStateLock()) {
    return;
  }
  g_opState.bleConectado = connected;
  Serial.printf("[BLE] Connection state changed: %s | opState=%d | session=%s\n",
                connected ? "CONNECTED" : "DISCONNECTED",
                g_opState.state,
                g_opState.sessionId.c_str());
  if (!connected) {
    g_opState.lastDisconnectMs = millis();
    if (g_opState.state == RUNNING) {
      g_resumeSessionOnConnect = true;
    } else {
      g_opState.autenticado = false;
      g_opState.sessionId = "";
      g_opState.currentCmdId = "";
      g_opState.state = IDLE;
      g_opState.ready = false;
      g_opState.mtuAtualizado = false;
      g_opState.readyAtMs = 0;
      g_resumeSessionOnConnect = false;
    }
  } else {
    g_opState.lastConnectMs = millis();
    if (g_resumeSessionOnConnect && g_opState.state == RUNNING) {
      g_opState.ready = true;
      g_opState.readyAtMs = g_opState.lastConnectMs;
    } else {
      g_opState.ready = false;
      g_opState.readyAtMs = 0;
      g_opState.mtuAtualizado = false;
    }
    g_resumeSessionOnConnect = false;
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
  Serial.printf("[BLE] Comando recebido, enfileirando\n");
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
    g_advBackoffMs = BLE_ADV_BACKOFF_START_MS;
    g_advPending = false;
    Serial.println("DEVICE CONNECTED");
    Serial.printf("[BLE] Firmware v%s (build %s %s)\n",
                  FW_VERSION, FW_BUILD_DATE, FW_BUILD_TIME);
  }

  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) {
    (void)server;
    if (param) {
      setConnectionState(true);
      g_advBackoffMs = BLE_ADV_BACKOFF_START_MS;
      g_advPending = false;
      Serial.println("DEVICE CONNECTED");
      Serial.printf("[BLE] Firmware v%s (build %s %s)\n",
                    FW_VERSION, FW_BUILD_DATE, FW_BUILD_TIME);

      // v2.4.2 FIX: Removido esp_ble_gap_update_conn_params daqui.
      // O Android (BluetoothServiceIndustrial) negocia requestConnectionPriority(BALANCED)
      // que define timeout ~20s. Forçar qualquer timeout aqui sobrescrevia essa negociação
      // e causava desconexão (GATT status 133) durante a dispensação.
      // O ESP32 agora aceita passivamente os parâmetros propostos pelo Android.
      Serial.println("[BLE] Conectado — aguardando Android negociar conn params (BALANCED ~20s)");
    } else {
      onConnect(server);
    }
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    setConnectionState(false);
    uint32_t sinceConnect = 0;
    uint32_t sinceReady = 0;
    bool hadReady = false;
    if (opStateLock()) {
      if (g_opState.lastConnectMs > 0) {
        sinceConnect = millis() - g_opState.lastConnectMs;
      }
      if (g_opState.ready) {
        hadReady = true;
        sinceReady = millis() - g_opState.readyAtMs;
      }
      opStateUnlock();
    }

    Serial.printf("DEVICE DISCONNECTED | since_connect=%lu ms%s\n",
                  static_cast<unsigned long>(sinceConnect),
                  hadReady ? String(" | since_ready=" + String(sinceReady) + " ms").c_str() : "");
    valveController_abortFromBleDisconnect();

    g_nextAdvertiseMs = millis() + g_advBackoffMs;
    g_advPending = true;
    g_advBackoffMs = (g_advBackoffMs < BLE_ADV_BACKOFF_MAX_MS)
        ? min(g_advBackoffMs * 2UL, BLE_ADV_BACKOFF_MAX_MS)
        : BLE_ADV_BACKOFF_MAX_MS;
  }

  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) {
    if (param) {
      Serial.printf("GATT DISCONNECT reason=0x%02X\n", param->disconnect.reason);
    } else {
      Serial.println("GATT DISCONNECT reason=UNKNOWN");
    }
    onDisconnect(server);
  }

  void onMtuChanged(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    (void)server;
    uint16_t mtu = param ? param->mtu.mtu : 0;
    if (opStateLock()) {
      g_opState.mtuAtualizado = true;
      opStateUnlock();
    }
    Serial.printf("MTU CHANGED: %u\n", mtu);
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

  const uint32_t nowMs = millis();
  if (nowMs - g_lastTxMs < BLE_GATT_TX_GUARD_MS) {
    vTaskDelay(pdMS_TO_TICKS(BLE_GATT_TX_GUARD_MS - (nowMs - g_lastTxMs)));
  }

  Serial.printf("TX: %s\n", payload.c_str());
  g_txCharacteristic->setValue(payload.c_str());
  g_txCharacteristic->notify();
  g_lastTxMs = millis();
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
  g_advPending = false;
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

  if (g_advPending) {
    if (millis() >= g_nextAdvertiseMs) {
      g_advertising->start();
      g_lastAdvertisingStart = millis();
      g_advPending = false;
    }
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
    bleProtocol_restartAdvertisingIfNeeded();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}



