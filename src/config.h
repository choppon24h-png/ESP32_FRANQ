#pragma once

#include <Arduino.h>

// ============================================================
// VERSAO DO FIRMWARE
// ============================================================
// Historico:
//   2.0.0 - versao base
//   2.1.0 - FIX valvula abre/fecha, BLE reconexao ACKED, timeouts
//   2.2.0 - FIX: supervision timeout 5s->8s para ESP32-C3 single-core
//           FIX: taskYIELD no loop de dispensacao para liberar BLE
//           FIX: DISPENSE_LOOP_DELAY_MS 20ms->50ms
#define FW_VERSION    "2.2.0"
#define FW_BUILD_DATE __DATE__
#define FW_BUILD_TIME __TIME__

// ============================================================
// CONFIGURACOES GERAIS
// ============================================================
#define SERIAL_BAUDRATE 115200

// ============================================================
// CONFIGURACOES BLE
// ============================================================
#define BLE_DEVICE_PREFIX      "CHOPP_"
#define BLE_AUTH_TOKEN_MIN_LEN 8
#define BLE_ENABLE_BONDING     0

// ============================================================
// CONFIGURACOES DE FILA E HISTORICO
// ============================================================
#define COMMAND_QUEUE_SIZE   8
#define COMMAND_HISTORY_SIZE 32

// ============================================================
// CONFIGURACOES DE BUFFER
// ============================================================
#define RX_BUFFER_SIZE 160
#define TX_BUFFER_SIZE 160

// ============================================================
// CONFIGURACOES DE TEMPO BLE
// ============================================================
#define BLE_WATCHDOG_RESTART_MS    10000UL
#define BLE_READY_GUARD_MS         900UL
#define BLE_GATT_TX_GUARD_MS       30UL
#define BLE_ADV_BACKOFF_START_MS   500UL
#define BLE_ADV_BACKOFF_MAX_MS     5000UL

// ============================================================
// CONFIGURACOES DE DISPENSACAO
// ============================================================
#define DISPENSE_LOOP_DELAY_MS     50UL  // v2.2.0: 50ms dá mais tempo ao BLE no C3 single-core
#define DISPENSE_TIMEOUT_MS        30000UL  // 30s timeout absoluto de seguranca
#define DISPENSE_TIME_FOR_300ML_MS 8000UL   // v2.1.0 FIX: era 2000UL

// ============================================================
// CONFIGURACOES DO SENSOR DE FLUXO
// ============================================================
#define FLOW_NO_PULSE_TIMEOUT_MS   10000UL  // v2.1.0 FIX: era 3000UL
#define FLOW_MIN_OPEN_MS           10000UL  // v2.1.0 NOVO: 10s minimo aberta antes de checar sensor
#define FLOW_PULSOS_POR_LITRO      450

// ============================================================
// CONFIGURACOES DE AUTENTICACAO HMAC
// ============================================================
#define AUTH_SECRET_KEY     "Choppon103614@"
#define AUTH_TOKEN_VALID_MS 300000UL

// ============================================================
// CONFIGURACOES NVS (PERSISTENCIA EM FLASH)
// ============================================================
#define NVS_NAMESPACE   "chopp_hist"
#define NVS_KEY_HISTORY "cmd_hist"

// ============================================================
// CONFIGURACOES DO RELE
// ============================================================
#define RELE_ON LOW

// ============================================================
// PINAGEM POR PLACA
// ============================================================
#if defined(ARDUINO_LOLIN_C3_MINI) || defined(CONFIG_IDF_TARGET_ESP32C3)
  #define PINO_SENSOR_FLUSO 0
  #define PINO_RELE         1
  #define PINO_STATUS       8
  #define LED_STATUS_ON     LOW
#elif defined(ARDUINO_ESP32S3_DEV)
  #define PINO_SENSOR_FLUSO 2
  #define PINO_RELE         48
  #define PINO_STATUS       21
  #define LED_STATUS_ON     HIGH
#else
  // ESP32 DevKit padrao
  #define PINO_SENSOR_FLUSO 17
  #define PINO_RELE         16
  #define PINO_STATUS       2
  #define LED_STATUS_ON     HIGH
#endif
