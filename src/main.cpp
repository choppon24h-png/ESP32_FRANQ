#include <Arduino.h>

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

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUDRATE);
  delay(250);

  printBanner();

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

