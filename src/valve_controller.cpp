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
  if (xSemaphoreTake(g_dispenseMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (g_running) {
      g_request.stopRequested = true;
      g_request.abortRequested = true;
    }
    xSemaphoreGive(g_dispenseMutex);
  }

  if (opStateLock()) {
    if (g_opState.state == RUNNING) {
      g_opState.state = IDLE;
      g_opState.currentCmdId = "";
      g_opState.sessionId = "";
    }
    opStateUnlock();
  }

  fecharValvula();
  flowSensor_disable();
  Serial.println("[VALVE] BLE_ABORT — válvula fechada por desconexão");
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

    Serial.printf("[VALVE] SERVE_START — alvo: %u ml\n", local.targetMl);

    // Habilitar sensor de fluxo
    flowSensor_enable();
    const uint32_t alvoPulsos = flowSensor_calcularAlvo(local.targetMl);
    Serial.printf("[FLOW] Alvo: %u pulsos (%u ml)\n", alvoPulsos, local.targetMl);

    // Timeout de segurança = 3x o tempo esperado, limitado ao máximo
    const uint32_t expectedMs = (local.targetMl > 0)
        ? (uint32_t)((uint64_t)local.targetMl * DISPENSE_TIME_FOR_300ML_MS / 300ULL) * 3UL
        : DISPENSE_TIMEOUT_MS;
    const uint32_t safeTimeout = (expectedMs < DISPENSE_TIMEOUT_MS) ? expectedMs : DISPENSE_TIMEOUT_MS;

    const uint32_t startMs = millis();
    uint32_t actualMl = 0;
    bool stopped = false;
    bool abortedByDisconnect = false;
    bool flowTimeout = false;

    // Loop principal de dispensação por sensor de fluxo
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(DISPENSE_LOOP_DELAY_MS));

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

      // Timeout de segurança absoluto
      if ((millis() - startMs) >= safeTimeout) {
        actualMl = flowSensor_getMl();
        Serial.printf("[VALVE] Timeout de segurança. Dispensado: %u ml\n", actualMl);
        break;
      }

      // Timeout de fluxo: sensor parou de pulsar (barril vazio ou entupimento)
      // Só verifica após os primeiros pulsos (para não disparar no início)
      if (flowSensor_getPulsos() > 5 && flowSensor_isTimeout()) {
        actualMl = flowSensor_getMl();
        flowTimeout = true;
        Serial.printf("[FLOW] Timeout de fluxo — barril vazio? Dispensado: %u ml\n", actualMl);
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
