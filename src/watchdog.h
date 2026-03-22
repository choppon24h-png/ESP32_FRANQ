#pragma once

#include <Arduino.h>

/**
 * watchdog — Watchdog de hardware + software para alta disponibilidade.
 *
 * Funcionalidades:
 *   - Watchdog de hardware via esp_task_wdt (reinicia a placa se travar por >30s)
 *   - Reinício de advertising BLE quando desconectado
 *   - Monitoramento de uptime e status do sistema
 */

void watchdog_init();

void watchdogTask(void* param);

uint32_t watchdog_getUptimeSeconds();
