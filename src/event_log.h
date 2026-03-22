#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// MÓDULO: event_log.h — Log Interno de Eventos (v2.2)
// ═══════════════════════════════════════════════════════════════════════════
//
// Sistema de log circular em RAM para rastreabilidade de eventos críticos.
// Capacidade: 32 eventos (circular — sobrescreve os mais antigos).
// Eventos podem ser lidos via comando $LOGS e enviados ao Android via BLE.
//
// EVENTOS REGISTRADOS:
//   serve_start          — Início de dispensação
//   serve_complete       — Dispensação concluída com sucesso
//   serve_timeout        — Dispensação encerrada por timeout
//   serve_stop           — Dispensação encerrada por comando STOP
//   ble_connected        — Android conectou
//   ble_disconnected     — Android desconectou
//   ble_watchdog_trigger — Watchdog BLE disparou (30s sem comunicação)
//   duplicate_command    — Comando duplicado recebido e ignorado
//   auth_ok              — Autenticação bem-sucedida
//   auth_fail            — Falha de autenticação
//   valve_timeout        — Válvula fechada por timeout de segurança (10s)
//   wdg_ping_timeout     — Válvula fechada por falta de PING (5s)
// ═══════════════════════════════════════════════════════════════════════════

#include "Arduino.h"

// ── Tamanho do buffer circular ────────────────────────────────────────────
#define EVENT_LOG_SIZE  32
#define EVENT_MSG_LEN   64

// ── Estrutura de um evento ────────────────────────────────────────────────
typedef struct {
    uint64_t timestamp_ms;          // Timestamp em ms desde boot
    char     event[EVENT_MSG_LEN];  // Descrição do evento
} EventEntry;

// ── API pública ───────────────────────────────────────────────────────────

/**
 * Inicializa o módulo de log interno.
 * Deve ser chamado em setup().
 */
void eventLog_init();

/**
 * Registra um evento no log circular.
 * @param event  String descritiva do evento (ex: "serve_start|ml=300")
 */
void eventLog_record(const char* event);

/**
 * Retorna o número de eventos registrados (máximo EVENT_LOG_SIZE).
 */
uint16_t eventLog_count();

/**
 * Lê um evento pelo índice (0 = mais antigo, count-1 = mais recente).
 * @param index  Índice do evento
 * @param out    Buffer de saída (deve ter pelo menos EVENT_MSG_LEN bytes)
 * @return true se o índice é válido
 */
bool eventLog_get(uint16_t index, EventEntry* out);

/**
 * Limpa todos os eventos do log.
 */
void eventLog_clear();

/**
 * Envia todos os eventos via BLE (para diagnóstico remoto).
 * Chamado pelo handler do comando $LOGS.
 */
void eventLog_sendViaBLE();
