#pragma once

#include <Arduino.h>

void bleProtocol_init();
void bleProtocol_send(const String& payload);
void bleProtocol_startAdvertising();
void bleProtocol_restartAdvertisingIfNeeded();
String bleProtocol_getDeviceName();
bool bleProtocol_isConnected();

void taskBLE(void* param);
