#pragma once

#include <Arduino.h>

#include "protocol.h"

void cmdHistory_init();
bool isDuplicate(const String& cmdId);
bool getResult(const String& cmdId, HistoryEntry& result);
void cmdHistory_markPending(const String& cmdId, const String& sessionId);
void cmdHistory_markDone(const String& cmdId, const String& sessionId, uint32_t mlReal);
