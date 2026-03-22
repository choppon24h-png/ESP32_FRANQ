#include "watchdog.h"

#include <esp_task_wdt.h>

#include "ble_protocol.h"
#include "config.h"
#include "protocol.h"

// Timeout do watchdog de hardware em segundos
#define HW_WDT_TIMEOUT_S 30

namespace {

uint32_t g_bootMs = 0;

}  // namespace

void watchdog_init() {
  g_bootMs = millis();

  // Inicializar watchdog de hardware.
  // A API esp_task_wdt_config_t foi introduzida no ESP-IDF v5.x.
  // Para compatibilidade com esp32-arduino baseado em IDF v4.x,
  // usamos a API legada esp_task_wdt_init(timeout, panic).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t wdtCfg = {
      .timeout_ms    = HW_WDT_TIMEOUT_S * 1000U,
      .idle_core_mask = 0,
      .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdtCfg);
#else
  // API legada IDF v4.x: esp_task_wdt_init(timeout_segundos, panic)
  esp_task_wdt_init(HW_WDT_TIMEOUT_S, true);
#endif

  Serial.printf("[WDT] Watchdog de hardware inicializado. Timeout: %d s\n", HW_WDT_TIMEOUT_S);
}

uint32_t watchdog_getUptimeSeconds() {
  return (millis() - g_bootMs) / 1000UL;
}

void watchdogTask(void* param) {
  (void)param;

  // Registrar esta task no watchdog de hardware
  esp_task_wdt_add(NULL);

  Serial.println("[WDT] Task watchdog iniciada");

  for (;;) {
    // Reset do watchdog de hardware (prova que a task está viva)
    esp_task_wdt_reset();

    // Reiniciar advertising BLE se desconectado
    bleProtocol_restartAdvertisingIfNeeded();

    // Log de status periódico a cada 60 segundos
    static uint32_t lastStatusLog = 0;
    const uint32_t now = millis();
    if ((now - lastStatusLog) >= 60000UL) {
      lastStatusLog = now;

      bool connected = false;
      bool authenticated = false;
      bool running = false;

      if (opStateLock()) {
        connected = g_opState.bleConectado;
        authenticated = g_opState.autenticado;
        running = (g_opState.state == RUNNING);
        opStateUnlock();
      }

      Serial.printf("[WDT] Status — Uptime: %lu s | BLE: %s | Auth: %s | Dispensando: %s\n",
                    watchdog_getUptimeSeconds(),
                    connected ? "CONECTADO" : "DESCONECTADO",
                    authenticated ? "SIM" : "NAO",
                    running ? "SIM" : "NAO");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
