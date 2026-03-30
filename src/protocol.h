#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "config.h"

enum State {
  IDLE = 0,
  READY = 1,
  RUNNING = 2,
  ERROR = 3
};

struct OperationState {
  bool bleConectado;
  bool autenticado;
  String sessionId;
  String currentCmdId;
  State state;
  bool ready;
  bool mtuAtualizado;
  uint32_t readyAtMs;
  uint32_t lastConnectMs;
  uint32_t lastDisconnectMs;
};

struct ParsedCommand {
  String name;
  String param;
  String cmdId;
  String sessionId;
  bool valid;
};

struct QueuedCommand {
  char raw[RX_BUFFER_SIZE];
};

struct HistoryEntry {
  char cmdId[48];
  char sessionId[48];
  uint32_t mlReal;
  bool done;
  bool occupied;
};

extern OperationState g_opState;
extern SemaphoreHandle_t g_opStateMutex;

inline bool opStateLock(TickType_t timeout = pdMS_TO_TICKS(100)) {
  return xSemaphoreTake(g_opStateMutex, timeout) == pdTRUE;
}

inline void opStateUnlock() {
  xSemaphoreGive(g_opStateMutex);
}

inline void opStateReset() {
  if (opStateLock()) {
    g_opState.bleConectado = false;
    g_opState.autenticado = false;
    g_opState.sessionId = "";
    g_opState.currentCmdId = "";
    g_opState.state = IDLE;
    g_opState.ready = false;
    g_opState.mtuAtualizado = false;
    g_opState.readyAtMs = 0;
    g_opState.lastConnectMs = 0;
    g_opState.lastDisconnectMs = 0;
    opStateUnlock();
  }
}
