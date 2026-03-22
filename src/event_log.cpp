#include "event_log.h"
#include "ble_protocol.h"
#include "protocol.h"
#include <string.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: event_log.cpp — Log Interno de Eventos (v2.2)
// ═══════════════════════════════════════════════════════════════════════════

// ── Buffer circular ───────────────────────────────────────────────────────
static EventEntry     s_log[EVENT_LOG_SIZE];
static uint16_t       s_head  = 0;   // Próxima posição de escrita
static uint16_t       s_count = 0;   // Total de entradas válidas
static SemaphoreHandle_t s_mutex = nullptr;

// ── Inicialização ─────────────────────────────────────────────────────────
void eventLog_init() {
    memset(s_log, 0, sizeof(s_log));
    s_head  = 0;
    s_count = 0;
    s_mutex = xSemaphoreCreateMutex();
    DBG_PRINTLN("[EVTLOG] Módulo de log de eventos inicializado (cap=" STRINGIFY(EVENT_LOG_SIZE) ")");
}

// ── Registra um evento ────────────────────────────────────────────────────
void eventLog_record(const char* event) {
    if (!event) return;

    uint64_t ts = (uint64_t)esp_timer_get_time() / 1000ULL;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_log[s_head].timestamp_ms = ts;
        strncpy(s_log[s_head].event, event, EVENT_MSG_LEN - 1);
        s_log[s_head].event[EVENT_MSG_LEN - 1] = '\0';

        s_head = (s_head + 1) % EVENT_LOG_SIZE;
        if (s_count < EVENT_LOG_SIZE) s_count++;

        xSemaphoreGive(s_mutex);
    }

    // Sempre imprime no Serial para diagnóstico local
    DBG_PRINTF("[EVT] %llu | %s\n", ts, event);
}

// ── Retorna o número de eventos ───────────────────────────────────────────
uint16_t eventLog_count() {
    return s_count;
}

// ── Lê um evento pelo índice ──────────────────────────────────────────────
// Índice 0 = mais antigo, count-1 = mais recente
bool eventLog_get(uint16_t index, EventEntry* out) {
    if (!out || index >= s_count) return false;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Calcula a posição real no buffer circular
        // s_head aponta para a PRÓXIMA posição de escrita (= mais antigo se cheio)
        uint16_t oldest = (s_count < EVENT_LOG_SIZE) ? 0 : s_head;
        uint16_t pos = (oldest + index) % EVENT_LOG_SIZE;
        *out = s_log[pos];
        xSemaphoreGive(s_mutex);
        return true;
    }
    return false;
}

// ── Limpa o log ───────────────────────────────────────────────────────────
void eventLog_clear() {
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memset(s_log, 0, sizeof(s_log));
        s_head  = 0;
        s_count = 0;
        xSemaphoreGive(s_mutex);
    }
    DBG_PRINTLN("[EVTLOG] Log limpo");
}

// ── Envia todos os eventos via BLE ────────────────────────────────────────
void eventLog_sendViaBLE() {
    uint16_t total = eventLog_count();
    if (total == 0) {
        bleProtocol_send("LOGS:EMPTY");
        return;
    }

    char buf[PROTO_TX_BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "LOGS:COUNT=%u", total);
    bleProtocol_send(buf);

    for (uint16_t i = 0; i < total; i++) {
        EventEntry entry;
        if (eventLog_get(i, &entry)) {
            snprintf(buf, sizeof(buf), "LOG[%u]:%llu|%s", i, entry.timestamp_ms, entry.event);
            bleProtocol_send(buf);
            vTaskDelay(pdMS_TO_TICKS(20)); // Evita flood BLE
        }
    }

    bleProtocol_send("LOGS:END");
}
