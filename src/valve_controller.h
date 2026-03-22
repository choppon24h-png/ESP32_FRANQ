#pragma once

#include <Arduino.h>

/**
 * valve_controller — Controle da válvula e lógica de dispensação.
 *
 * A dispensação usa o sensor de fluxo YF-S401 para medir o volume real.
 * Fallback temporizado é usado apenas se o sensor não detectar pulsos.
 */

void valveController_init();

bool valveController_startDispensacao(uint32_t ml,
                                       const String& cmdId,
                                       const String& sessionId);

void valveController_stop(const String& cmdId, const String& sessionId);

void valveController_abortFromBleDisconnect();

bool valveController_isRunning();

void abrirValvula();

void fecharValvula();

void taskDispensacao(void* param);
