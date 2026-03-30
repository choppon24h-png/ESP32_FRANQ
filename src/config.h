#pragma once

#include <Arduino.h>

// ============================================================
// VERSÃƒO DO FIRMWARE
// ============================================================
#define FW_VERSION "2.0.0"

// ============================================================
// CONFIGURAÃ‡Ã•ES GERAIS
// ============================================================
#define SERIAL_BAUDRATE 115200

// ============================================================
// CONFIGURAÃ‡Ã•ES BLE
// ============================================================
#define BLE_DEVICE_PREFIX      "CHOPP_"
#define BLE_AUTH_TOKEN_MIN_LEN 8
#define BLE_ENABLE_BONDING     0

// ============================================================
// CONFIGURAÃ‡Ã•ES DE FILA E HISTÃ“RICO
// ============================================================
#define COMMAND_QUEUE_SIZE   8
#define COMMAND_HISTORY_SIZE 32

// ============================================================
// CONFIGURAÃ‡Ã•ES DE BUFFER
// ============================================================
#define RX_BUFFER_SIZE 160
#define TX_BUFFER_SIZE 160

// ============================================================
// CONFIGURAÃ‡Ã•ES DE TEMPO
// ============================================================
#define BLE_WATCHDOG_RESTART_MS    10000UL
#define BLE_READY_GUARD_MS         900UL   // 800-1000ms de guard-band apÃƒÂ³s READY
#define BLE_GATT_TX_GUARD_MS       30UL    // espaÃƒÂ§amento mÃƒÂ­nimo entre notifies
#define BLE_ADV_BACKOFF_START_MS   500UL
#define BLE_ADV_BACKOFF_MAX_MS     5000UL
#define DISPENSE_LOOP_DELAY_MS     20UL
#define DISPENSE_TIMEOUT_MS        30000UL  // 30s mÃ¡ximo de seguranÃ§a
#define DISPENSE_TIME_FOR_300ML_MS 8000UL  // 8s estimativa real para 300ml

// ============================================================
// CONFIGURAÃ‡Ã•ES DE AUTENTICAÃ‡ÃƒO HMAC
// ============================================================
// Chave secreta compartilhada entre firmware e app Android.
// O Android gera HMAC-SHA256(SESSION_ID + timestamp, AUTH_SECRET_KEY).
// IMPORTANTE: altere esta chave em cada franquia para seguranÃ§a mÃ¡xima.
#define AUTH_SECRET_KEY     "Choppon103614@"
#define AUTH_TOKEN_VALID_MS 300000UL  // Token vÃ¡lido por 5 minutos

// ============================================================
// CONFIGURAÃ‡Ã•ES DO SENSOR DE FLUXO
// ============================================================
// Pulsos por litro do sensor YF-S401 (calibrar por lote de sensor)
#define FLOW_PULSOS_POR_LITRO    450
// Timeout de fluxo: se nÃ£o detectar pulso em X ms, considera vazio/entupido
#define FLOW_NO_PULSE_TIMEOUT_MS 10000UL  // 10s — tempo mínimo para pressurização inicial
#define FLOW_MIN_OPEN_MS 10000UL  // Válvula fica aberta no mínimo 10s antes de checar timeout de fluxo

// ============================================================
// CONFIGURAÃ‡Ã•ES NVS (PERSISTÃŠNCIA EM FLASH)
// ============================================================
#define NVS_NAMESPACE   "chopp_hist"
#define NVS_KEY_HISTORY "cmd_hist"

// ============================================================
// CONFIGURAÃ‡Ã•ES DO RELÃ‰
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
  // ESP32 DevKit padrÃ£o
  #define PINO_SENSOR_FLUSO 17
  #define PINO_RELE         16
  #define PINO_STATUS       2
  #define LED_STATUS_ON     HIGH
#endif

