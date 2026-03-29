#pragma once

#include <Arduino.h>

// ============================================================
// VERSÃO DO FIRMWARE
// ============================================================
#define FW_VERSION "2.0.0"

// ============================================================
// CONFIGURAÇÕES GERAIS
// ============================================================
#define SERIAL_BAUDRATE 115200

// ============================================================
// CONFIGURAÇÕES BLE
// ============================================================
#define BLE_DEVICE_PREFIX      "CHOPP_"
#define BLE_AUTH_TOKEN_MIN_LEN 8
#define BLE_ENABLE_BONDING     0

// ============================================================
// CONFIGURAÇÕES DE FILA E HISTÓRICO
// ============================================================
#define COMMAND_QUEUE_SIZE   8
#define COMMAND_HISTORY_SIZE 32

// ============================================================
// CONFIGURAÇÕES DE BUFFER
// ============================================================
#define RX_BUFFER_SIZE 160
#define TX_BUFFER_SIZE 160

// ============================================================
// CONFIGURAÇÕES DE TEMPO
// ============================================================
#define BLE_WATCHDOG_RESTART_MS    10000UL
#define BLE_READY_GUARD_MS         900UL   // 800-1000ms de guard-band apÃ³s READY
#define BLE_GATT_TX_GUARD_MS       30UL    // espaÃ§amento mÃ­nimo entre notifies
#define BLE_ADV_BACKOFF_START_MS   500UL
#define BLE_ADV_BACKOFF_MAX_MS     5000UL
#define DISPENSE_LOOP_DELAY_MS     20UL
#define DISPENSE_TIMEOUT_MS        30000UL  // 30s máximo de segurança
#define DISPENSE_TIME_FOR_300ML_MS 2000UL   // Fallback se sensor falhar

// ============================================================
// CONFIGURAÇÕES DE AUTENTICAÇÃO HMAC
// ============================================================
// Chave secreta compartilhada entre firmware e app Android.
// O Android gera HMAC-SHA256(SESSION_ID + timestamp, AUTH_SECRET_KEY).
// IMPORTANTE: altere esta chave em cada franquia para segurança máxima.
#define AUTH_SECRET_KEY     "Choppon103614@"
#define AUTH_TOKEN_VALID_MS 300000UL  // Token válido por 5 minutos

// ============================================================
// CONFIGURAÇÕES DO SENSOR DE FLUXO
// ============================================================
// Pulsos por litro do sensor YF-S401 (calibrar por lote de sensor)
#define FLOW_PULSOS_POR_LITRO    450
// Timeout de fluxo: se não detectar pulso em X ms, considera vazio/entupido
#define FLOW_NO_PULSE_TIMEOUT_MS 3000UL

// ============================================================
// CONFIGURAÇÕES NVS (PERSISTÊNCIA EM FLASH)
// ============================================================
#define NVS_NAMESPACE   "chopp_hist"
#define NVS_KEY_HISTORY "cmd_hist"

// ============================================================
// CONFIGURAÇÕES DO RELÉ
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
  // ESP32 DevKit padrão
  #define PINO_SENSOR_FLUSO 17
  #define PINO_RELE         16
  #define PINO_STATUS       2
  #define LED_STATUS_ON     HIGH
#endif
