#include "valve_controller.h"

#include "ble_protocol.h"
#include "command_history.h"
#include "config.h"
#include "flow_sensor.h"
#include "protocol.h"

namespace {

struct DispenseRequest {
  uint32_t targetMl;
  String cmdId;
  String sessionId;
  bool stopRequested;
  bool abortRequested;
};

SemaphoreHandle_t g_dispenseMutex = nullptr;
SemaphoreHandle_t g_dispenseStart = nullptr;
DispenseRequest g_request;
bool g_running = false;
bool g_valveOpen = false;

void relayWrite(bool open) {
  digitalWrite(PINO_RELE, open ? RELE_ON : !RELE_ON);
}

void resetOperationalStateLocked() {
  g_opState.sessionId = "";
  g_opState.currentCmdId = "";
  g_opState.state = IDLE;
  g_opState.ready = false;
  g_opState.autenticado = true; // mantém autenticação de sessão para reconexão, mas não permite novo SERVE sem READY
}

void clearRequestLocked() {
  g_request = {};
  g_running = false;
}

void finalizeDispense(const DispenseRequest& local, uint32_t actualMl, bool notifyDone) {
  fecharValvula();
  flowSensor_disable();
  cmdHistory_markDone(local.cmdId, local.sessionId, actualMl);

  if (opStateLock()) {
    resetOperationalStateLocked();
    opStateUnlock();
  }

  Serial.printf("[VALVE] Finalizado. Pedido: %u ml | Real: %u ml\n",
                local.targetMl, actualMl);

  if (notifyDone && bleProtocol_isConnected()) {
    bleProtocol_send(String("DONE|") + local.cmdId + "|" + String(actualMl) + "|" + local.sessionId);
    Serial.println("[VALVE] DONE_SENT");
  }

  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    clearRequestLocked();
    xSemaphoreGive(g_dispenseMutex);
  }
}

}  // namespace

void valveController_init() {
  pinMode(PINO_RELE, OUTPUT);
  relayWrite(false);

  g_dispenseMutex = xSemaphoreCreateMutex();
  g_dispenseStart = xSemaphoreCreateBinary();
  configASSERT(g_dispenseMutex != nullptr);
  configASSERT(g_dispenseStart != nullptr);

  flowSensor_init();
  Serial.println("[VALVE] Controlador inicializado com sensor de fluxo real");
}

bool valveController_startDispensacao(uint32_t ml, const String& cmdId, const String& sessionId) {
  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  if (g_running) {
    xSemaphoreGive(g_dispenseMutex);
    return false;
  }

  g_request.targetMl = ml;
  g_request.cmdId = cmdId;
  g_request.sessionId = sessionId;
  g_request.stopRequested = false;
  g_request.abortRequested = false;
  g_running = true;

  xSemaphoreGive(g_dispenseMutex);
  xSemaphoreGive(g_dispenseStart);
  return true;
}

void abrirValvula() {
  if (g_valveOpen) return;
  relayWrite(true);
  g_valveOpen = true;
  Serial.println("[VALVE] VALVE_OPEN");
}

void fecharValvula() {
  relayWrite(false);
  g_valveOpen = false;
  Serial.println("[VALVE] VALVE_CLOSE");
}

void valveController_stop(const String& cmdId, const String& sessionId) {
  (void)cmdId;
  (void)sessionId;
  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  g_request.stopRequested = true;
  const bool running = g_running;
  xSemaphoreGive(g_dispenseMutex);

  if (!running) {
    fecharValvula();
    flowSensor_disable();
    if (opStateLock()) {
      resetOperationalStateLocked();
      opStateUnlock();
    }
  }
}

void valveController_abortFromBleDisconnect() {
  bool running = false;
  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    running = g_running;
    xSemaphoreGive(g_dispenseMutex);
  }

  // Se estiver rodando OU a válvula estiver fisicamente aberta, NÃO aborta.
  if (running || g_valveOpen) {
    Serial.println("[VALVE] BLE disconnect durante SERVE — IGNORANDO ABORT, mantendo valvula aberta");
    return;
  }
  
  // Só fecha se realmente não estiver rodando e a válvula estiver fechada
  if (opStateLock()) {
    if (g_opState.state == RUNNING) {
      resetOperationalStateLocked();
    }
    opStateUnlock();
  }
  fecharValvula();
  flowSensor_disable();
  Serial.println("[VALVE] BLE abort — válvula fechada por desconexão (não estava rodando)");
}

bool valveController_isRunning() {
  bool running = false;
  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    running = g_running;
    xSemaphoreGive(g_dispenseMutex);
  }
  return running;
}

void taskDispensacao(void* param) {
  (void)param;

  for (;;) {
    xSemaphoreTake(g_dispenseStart, portMAX_DELAY);

    DispenseRequest local;
    if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }
    local = g_request;
    xSemaphoreGive(g_dispenseMutex);

    Serial.printf("[VALVE] SERVE_START â€” alvo: %u ml\n", local.targetMl);

    // Habilitar sensor de fluxo
    flowSensor_enable();
    const uint32_t alvoPulsos = flowSensor_calcularAlvo(local.targetMl);
    Serial.printf("[FLOW] Alvo: %u pulsos (%u ml)\n", alvoPulsos, local.targetMl);

    // Timeout de seguranÃ§a = 3x o tempo esperado, limitado ao mÃ¡ximo
    const uint32_t expectedMs = (local.targetMl > 0)
        ? (uint32_t)((uint64_t)local.targetMl * DISPENSE_TIME_FOR_300ML_MS / 300ULL) * 3UL
        : DISPENSE_TIMEOUT_MS;
    const uint32_t safeTimeout = (expectedMs < DISPENSE_TIMEOUT_MS) ? expectedMs : DISPENSE_TIMEOUT_MS;

    const uint32_t startMs = millis();
    uint32_t actualMl = 0;
    bool stopped = false;
    bool abortedByDisconnect = false;
    bool flowTimeout = false;

    // Loop principal de dispensaÃ§Ã£o por sensor de fluxo
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(DISPENSE_LOOP_DELAY_MS));
      taskYIELD();  // v2.2.0: cede CPU ao BLE stack no C3 single-core
      taskYIELD();  // v2.3.0: yield duplo garante que taskBLE/taskCmdProc rodem

      if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        stopped = g_request.stopRequested;
        abortedByDisconnect = g_request.abortRequested;
        xSemaphoreGive(g_dispenseMutex);
      }

      if (stopped) break;

      // Verificar volume pelo sensor
      const uint32_t currentMl = flowSensor_getMl();
      if (currentMl >= local.targetMl) {
        actualMl = currentMl;
        Serial.printf("[FLOW] Alvo atingido: %u ml\n", actualMl);
        break;
      }

      // Timeout de seguranca absoluto
      if ((millis() - startMs) >= safeTimeout) {
        actualMl = flowSensor_getMl();
        Serial.printf("[VALVE] Timeout de seguranca. Dispensado: %u ml\n", actualMl);
        break;
      }

      // v2.1.0 FIX: so verifica timeout de fluxo APOS FLOW_MIN_OPEN_MS (10s).
      // Garante tempo de pressurizacao da tubulacao antes de considerar
      // que o barril esta vazio ou ha entupimento.
      // v2.3.0 NOVO: log periodico de status a cada 5s para diagnostico
      const uint32_t elapsedMs = millis() - startMs;
      static uint32_t lastStatusLog = 0;
      if ((millis() - lastStatusLog) >= 5000UL) {
        lastStatusLog = millis();
        const uint32_t currentMl_log = flowSensor_getMl();
        const uint32_t pulsos_log = flowSensor_getPulsos();
        Serial.printf("[VALVE] STATUS elapsed=%lums ml=%u/%u pulsos=%u\n",
                      (unsigned long)elapsedMs, currentMl_log, local.targetMl, pulsos_log);
        if (bleProtocol_isConnected()) {
          bleProtocol_send(String("VP:") + String(currentMl_log));
        }
      }

      if (elapsedMs >= FLOW_MIN_OPEN_MS && flowSensor_getPulsos() > 5 && flowSensor_isTimeout()) {
        actualMl = flowSensor_getMl();
        flowTimeout = true;
        Serial.printf("[FLOW] v2.1.0 Timeout fluxo (elapsed=%lums) Dispensado: %u ml\n",
                      (unsigned long)elapsedMs, actualMl);
        break;
      }
    }

    // Parada manual: usar leitura real do sensor
    if (stopped && !abortedByDisconnect) {
      actualMl = flowSensor_getMl();
      Serial.printf("[VALVE] Parada manual. Volume real: %u ml\n", actualMl);
    }

    // Notificar app sobre barril vazio
    if (flowTimeout && bleProtocol_isConnected()) {
      bleProtocol_send(String("WARN:FLOW_TIMEOUT|") + local.cmdId + "|" + local.sessionId);
    }

    finalizeDispense(local, actualMl, !abortedByDisconnect);
  }
}




