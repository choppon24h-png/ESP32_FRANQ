#include <Arduino.h>
#include "esp_mac.h"       // esp_read_mac(), esp_iface_mac_addr_set() — FIX MAC BLE v2.4.0

#include "auth_validator.h"
#include "ble_protocol.h"
#include "command_history.h"
#include "command_parser.h"
#include "command_queue.h"
#include "config.h"
#include "protocol.h"
#include "valve_controller.h"
#include "watchdog.h"

OperationState g_opState;
SemaphoreHandle_t g_opStateMutex = nullptr;

namespace {

void createTaskCompat(TaskFunction_t fn,
                      const char* name,
                      uint32_t stackWords,
                      void* param,
                      UBaseType_t priority,
                      TaskHandle_t* handle,
                      BaseType_t preferredCore) {
#if portNUM_PROCESSORS > 1
  xTaskCreatePinnedToCore(fn, name, stackWords, param, priority, handle, preferredCore);
#else
  (void)preferredCore;
  xTaskCreate(fn, name, stackWords, param, priority, handle);
#endif
}

void initPins() {
  pinMode(PINO_STATUS, OUTPUT);
  digitalWrite(PINO_STATUS, !LED_STATUS_ON);
  pinMode(PINO_RELE, OUTPUT);
  digitalWrite(PINO_RELE, !RELE_ON);
}

void printBanner() {
  Serial.println();
  Serial.println("========================================");
  Serial.printf("  CHOPP FRANQUIA - Firmware v%s\n", FW_VERSION);
  Serial.printf("  Build: %s %s\n", FW_BUILD_DATE, FW_BUILD_TIME);
  Serial.println("  ESP32 BLE Controller");
  Serial.println("========================================");
  Serial.printf("  Pino Rele: %d | Pino Sensor: %d\n", PINO_RELE, PINO_SENSOR_FLUSO);
  Serial.printf("  Pulsos/Litro: %d | Timeout: %lu ms\n",
                FLOW_PULSOS_POR_LITRO, DISPENSE_TIMEOUT_MS);
  Serial.printf("  Min.abertura: %lu ms\n", FLOW_MIN_OPEN_MS);
  Serial.printf("  Timeout flux: %lu ms\n", FLOW_NO_PULSE_TIMEOUT_MS);
  Serial.println("========================================");
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────────
// fixBleMacToMatchWifi()
//
// PROBLEMA: No ESP32-C3, o MAC BLE é derivado do MAC base com incremento +2
// no último octeto. O banco de dados (chopponERP) armazena o MAC WiFi (STA).
// O Android tenta conectar via BLE usando o MAC WiFi cadastrado no banco, mas
// o rádio BLE responde apenas no MAC BLE (+2), causando falha de conexão
// (GATT status 133 / timeout) em todas as tentativas.
//
// SOLUÇÃO: Antes de inicializar qualquer interface de rede, forçamos o rádio
// BLE a usar exatamente o mesmo MAC da interface WiFi STA via
// esp_iface_mac_addr_set(). Assim, o MAC cadastrado no banco é o mesmo MAC
// que o Android usa para conectar via BLE — sem necessidade de alterar o
// backend, o banco de dados ou o aplicativo Android.
//
// REFERÊNCIA: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/
//             api-reference/system/misc_system_api.html#mac-address
// ─────────────────────────────────────────────────────────────────────────────
void fixBleMacToMatchWifi() {
  uint8_t wifiMac[6] = {0};
  uint8_t bleMac[6]  = {0};

  // Lê o MAC WiFi STA (o MAC cadastrado no banco de dados)
  esp_err_t err = esp_read_mac(wifiMac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    Serial.printf("[MAC-FIX] ERRO ao ler MAC WiFi: 0x%X — BLE usará MAC padrão\n", err);
    return;
  }

  // Lê o MAC BLE atual (antes da correção) para fins de log
  esp_read_mac(bleMac, ESP_MAC_BT);

  Serial.printf("[MAC-FIX] MAC WiFi STA : %02X:%02X:%02X:%02X:%02X:%02X\n",
                wifiMac[0], wifiMac[1], wifiMac[2],
                wifiMac[3], wifiMac[4], wifiMac[5]);
  Serial.printf("[MAC-FIX] MAC BLE orig : %02X:%02X:%02X:%02X:%02X:%02X\n",
                bleMac[0], bleMac[1], bleMac[2],
                bleMac[3], bleMac[4], bleMac[5]);

  // Força o rádio BLE a usar o mesmo MAC do WiFi STA
  err = esp_iface_mac_addr_set(wifiMac, ESP_MAC_BT);
  if (err != ESP_OK) {
    Serial.printf("[MAC-FIX] ERRO ao forcar MAC BLE: 0x%X — BLE usará MAC padrão\n", err);
    return;
  }

  // Confirma o MAC BLE após a correção
  esp_read_mac(bleMac, ESP_MAC_BT);
  Serial.printf("[MAC-FIX] MAC BLE fixo : %02X:%02X:%02X:%02X:%02X:%02X  OK\n",
                bleMac[0], bleMac[1], bleMac[2],
                bleMac[3], bleMac[4], bleMac[5]);
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUDRATE);
  delay(250);

  printBanner();

  // ─────────────────────────────────────────────────────────────────────────
  // v2.4.0 FIX: Forçar MAC BLE = MAC WiFi STA ANTES de qualquer inicialização
  // de interface de rede. Resolve a falha de conexão GATT (status 133)
  // causada pela diferença de +2 no último octeto entre MAC WiFi e MAC BLE
  // no ESP32-C3. O Android conecta via BLE usando o MAC WiFi do banco de dados.
  // ─────────────────────────────────────────────────────────────────────────
  fixBleMacToMatchWifi();

  // Mutex global de estado
  g_opStateMutex = xSemaphoreCreateMutex();
  configASSERT(g_opStateMutex != nullptr);

  initPins();
  opStateReset();

  // Inicializar mÃ³dulos em ordem
  authValidator_init();
  cmdHistory_init();    // Carrega histÃ³rico persistido da Flash (NVS)
  cmdQueue_init();
  valveController_init();  // Inclui flowSensor_init()
  watchdog_init();

  // Criar tasks FreeRTOS
  // v2.3.0 FIX: no C3 single-core, taskBLE e taskCommandProcessor DEVEM ter
  // prio MAIOR que taskDispensacao. Sem isso, enquanto a válvula está aberta,
  // o SERVE não consegue ser processado → ACK não sai → timeout → status=133.
  // Prioridades: taskBLE=5, taskCmdProc=5 > taskDispensacao=3, taskWatchdog=2
  createTaskCompat(taskBLE,              "taskBLE",      6144, nullptr, 5, nullptr, 0);
  createTaskCompat(taskCommandProcessor, "taskCmdProc",  6144, nullptr, 5, nullptr, 0);
#if portNUM_PROCESSORS > 1
  createTaskCompat(taskDispensacao,      "taskDispensa", 6144, nullptr, 3, nullptr, 1);
#else
  createTaskCompat(taskDispensacao,      "taskDispensa", 6144, nullptr, 3, nullptr, 0);
#endif
  createTaskCompat(watchdogTask,         "taskWatchdog", 4096, nullptr, 2, nullptr, 0);

  Serial.printf("[BOOT] v%s inicializado. Aguardando conexao BLE...\\n", FW_VERSION);
}

void loop() {
  // LED de heartbeat: 80ms ON / 920ms OFF
  digitalWrite(PINO_STATUS, LED_STATUS_ON);
  vTaskDelay(pdMS_TO_TICKS(80));
  digitalWrite(PINO_STATUS, !LED_STATUS_ON);
  vTaskDelay(pdMS_TO_TICKS(920));
}

