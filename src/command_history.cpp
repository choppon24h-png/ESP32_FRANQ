#include "command_history.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"

namespace {

HistoryEntry g_history[COMMAND_HISTORY_SIZE];
size_t g_historyIndex = 0;
SemaphoreHandle_t g_historyMutex = nullptr;
Preferences g_prefs;

// ── Helpers internos ─────────────────────────────────────────────────────

int findEntry(const String& cmdId) {
  for (size_t i = 0; i < COMMAND_HISTORY_SIZE; ++i) {
    if (g_history[i].occupied && cmdId.equals(g_history[i].cmdId)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int allocateEntry() {
  int index = static_cast<int>(g_historyIndex);
  g_historyIndex = (g_historyIndex + 1U) % COMMAND_HISTORY_SIZE;
  return index;
}

void copyToEntry(HistoryEntry& entry, const String& cmdId, const String& sessionId) {
  memset(&entry, 0, sizeof(entry));
  cmdId.substring(0, sizeof(entry.cmdId) - 1).toCharArray(entry.cmdId, sizeof(entry.cmdId));
  sessionId.substring(0, sizeof(entry.sessionId) - 1).toCharArray(entry.sessionId, sizeof(entry.sessionId));
  entry.occupied = true;
}

// ── Persistência NVS ─────────────────────────────────────────────────────

/**
 * Salva o histórico completo na memória Flash (NVS).
 * Chamado após cada modificação para garantir persistência.
 */
void saveToNvs() {
  if (!g_prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[HIST] ERRO: Falha ao abrir NVS para escrita");
    return;
  }

  // Salvar array de histórico como blob binário
  size_t written = g_prefs.putBytes(NVS_KEY_HISTORY, g_history, sizeof(g_history));
  // Salvar índice circular
  g_prefs.putUInt("hist_idx", (uint32_t)g_historyIndex);
  g_prefs.end();

  if (written == sizeof(g_history)) {
    Serial.printf("[HIST] Histórico salvo em NVS (%u bytes)\n", (unsigned)written);
  } else {
    Serial.printf("[HIST] AVISO: Escrita parcial em NVS (%u/%u bytes)\n",
                  (unsigned)written, (unsigned)sizeof(g_history));
  }
}

/**
 * Carrega o histórico da memória Flash (NVS) ao inicializar.
 */
void loadFromNvs() {
  if (!g_prefs.begin(NVS_NAMESPACE, true)) {
    Serial.println("[HIST] NVS não encontrado — iniciando histórico vazio");
    return;
  }

  size_t read = g_prefs.getBytes(NVS_KEY_HISTORY, g_history, sizeof(g_history));
  g_historyIndex = (size_t)g_prefs.getUInt("hist_idx", 0);
  g_prefs.end();

  if (read == sizeof(g_history)) {
    // Contar entradas válidas
    int count = 0;
    for (size_t i = 0; i < COMMAND_HISTORY_SIZE; i++) {
      if (g_history[i].occupied) count++;
    }
    Serial.printf("[HIST] Histórico restaurado da NVS: %d entradas válidas\n", count);
  } else {
    Serial.printf("[HIST] NVS incompleto (%u/%u bytes) — reiniciando histórico\n",
                  (unsigned)read, (unsigned)sizeof(g_history));
    memset(g_history, 0, sizeof(g_history));
    g_historyIndex = 0;
  }
}

}  // namespace

// ── API Pública ───────────────────────────────────────────────────────────

void cmdHistory_init() {
  memset(g_history, 0, sizeof(g_history));
  g_historyIndex = 0;

  if (!g_historyMutex) {
    g_historyMutex = xSemaphoreCreateMutex();
  }
  configASSERT(g_historyMutex != nullptr);

  // Carregar histórico persistido da Flash
  loadFromNvs();
}

bool isDuplicate(const String& cmdId) {
  if (cmdId.isEmpty()) return false;

  bool duplicate = false;
  if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    duplicate = findEntry(cmdId) >= 0;
    xSemaphoreGive(g_historyMutex);
  }
  return duplicate;
}

bool getResult(const String& cmdId, HistoryEntry& result) {
  if (cmdId.isEmpty()) return false;

  bool found = false;
  if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    const int index = findEntry(cmdId);
    if (index >= 0) {
      result = g_history[index];
      found = true;
    }
    xSemaphoreGive(g_historyMutex);
  }
  return found;
}

void cmdHistory_markPending(const String& cmdId, const String& sessionId) {
  if (cmdId.isEmpty()) return;

  if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  int index = findEntry(cmdId);
  if (index < 0) {
    index = allocateEntry();
  }

  copyToEntry(g_history[index], cmdId, sessionId);
  g_history[index].done = false;
  g_history[index].mlReal = 0;
  xSemaphoreGive(g_historyMutex);

  // Persistir imediatamente
  saveToNvs();
  Serial.printf("[HIST] Pendente registrado: %s\n", cmdId.c_str());
}

void cmdHistory_markDone(const String& cmdId, const String& sessionId, uint32_t mlReal) {
  if (cmdId.isEmpty()) return;

  if (xSemaphoreTake(g_historyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  int index = findEntry(cmdId);
  if (index < 0) {
    index = allocateEntry();
  }

  copyToEntry(g_history[index], cmdId, sessionId);
  g_history[index].done = true;
  g_history[index].mlReal = mlReal;
  xSemaphoreGive(g_historyMutex);

  // Persistir imediatamente
  saveToNvs();
  Serial.printf("[HIST] Concluído registrado: %s | %u ml\n", cmdId.c_str(), mlReal);
}
