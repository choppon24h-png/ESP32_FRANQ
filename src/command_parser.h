#pragma once

#include <Arduino.h>

#include "protocol.h"

bool commandParser_parse(const String& rawCommand, ParsedCommand& parsed);
void taskCommandProcessor(void* param);
