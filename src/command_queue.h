#pragma once

#include <Arduino.h>

void cmdQueue_init();
bool cmdQueue_enqueue(const String& rawCommand);
bool cmdQueue_dequeue(String& rawCommand, TickType_t timeoutTicks = portMAX_DELAY);
void cmdQueue_clear();
UBaseType_t cmdQueue_size();
