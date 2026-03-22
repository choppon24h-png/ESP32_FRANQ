#include "command_parser.h"

#include "auth_validator.h"
#include "ble_protocol.h"
#include "command_history.h"
#include "command_queue.h"
#include "config.h"
#include "valve_controller.h"

namespace {

String makeReply(const char* prefix, const String& cmdId, const String& sessionId) {
  String reply(prefix);
  if (!cmdId.isEmpty()) { reply += "|"; reply += cmdId; }
  if (!sessionId.isEmpty()) { reply += "|"; reply += sessionId; }
  return reply;
}

String makeDoneReply(const String& cmdId, uint32_t mlReal, const String& sessionId) {
  return String("DONE|") + cmdId + "|" + String(mlReal) + "|" + sessionId;
}

bool isSessionMismatch(const ParsedCommand& command, const HistoryEntry& entry) {
  return entry.occupied && !command.sessionId.equals(String(entry.sessionId));
}

bool isAuthenticated() {
  bool authorized = false;
  if (opStateLock()) {
    authorized = g_opState.autenticado;
    opStateUnlock();
  }
  return authorized;
}

bool hasActiveSessionMismatch(const ParsedCommand& command) {
  bool mismatch = false;
  if (opStateLock()) {
    mismatch = (g_opState.state == RUNNING &&
                !g_opState.sessionId.isEmpty() &&
                g_opState.sessionId != command.sessionId);
    opStateUnlock();
  }
  return mismatch;
}

bool requireAuthenticatedSession() {
  const bool authorized = isAuthenticated();
  if (!authorized) bleProtocol_send("ERROR:NOT_AUTHENTICATED");
  return authorized;
}

// ── Handlers ─────────────────────────────────────────────────────────────

void handleAuth(const ParsedCommand& command) {
  if (command.param.length() < BLE_AUTH_TOKEN_MIN_LEN || command.sessionId.isEmpty()) {
    Serial.println("[AUTH] Formato inválido");
    bleProtocol_send("ERROR:INVALID_AUTH");
    return;
  }

  // Validação HMAC-SHA256 do token
  if (!authValidator_validate(command.param, command.sessionId)) {
    Serial.printf("[AUTH] Token inválido: %s\n", authValidator_lastError());
    bleProtocol_send(String("ERROR:INVALID_TOKEN|") + authValidator_lastError());
    return;
  }

  if (!opStateLock()) { bleProtocol_send("ERROR:STATE_LOCK"); return; }
  g_opState.autenticado = true;
  g_opState.sessionId = command.sessionId;
  g_opState.currentCmdId = "";
  g_opState.state = IDLE;
  opStateUnlock();

  Serial.printf("[AUTH] Autenticado. Session: %s\n", command.sessionId.c_str());
  bleProtocol_send(makeReply("AUTH_OK", command.cmdId, command.sessionId));
}

void handlePing(const ParsedCommand& command) {
  (void)command.sessionId;
  bleProtocol_send(String("PONG|") + command.cmdId);
}

bool handleServeDuplicate(const ParsedCommand& command) {
  HistoryEntry prior = {};
  if (!getResult(command.cmdId, prior)) return false;

  Serial.printf("[DUP] Duplicata detectada CMD_ID=%s\n", command.cmdId.c_str());

  if (isSessionMismatch(command, prior)) {
    Serial.printf("[SEC] Session mismatch CMD_ID=%s\n", command.cmdId.c_str());
    bleProtocol_send("ERROR:SESSION_MISMATCH");
    return true;
  }

  if (prior.done) {
    Serial.println("[DUP] Resultado reaproveitado do histórico persistente");
    bleProtocol_send(makeDoneReply(command.cmdId, prior.mlReal, String(prior.sessionId)));
    return true;
  }

  bleProtocol_send(String("ACK|") + command.cmdId);
  return true;
}

void handleServe(const ParsedCommand& command) {
  if (hasActiveSessionMismatch(command)) {
    Serial.printf("[SEC] Session mismatch CMD_ID=%s\n", command.cmdId.c_str());
    bleProtocol_send("ERROR:SESSION_MISMATCH");
    return;
  }

  const uint32_t requestedMl = static_cast<uint32_t>(command.param.toInt());
  if (requestedMl == 0) { bleProtocol_send("ERROR:INVALID_ML"); return; }
  if (requestedMl > 2000) {
    Serial.printf("[SERVE] Volume excessivo: %u ml\n", requestedMl);
    bleProtocol_send("ERROR:VOLUME_EXCEEDED");
    return;
  }

  if (!opStateLock()) { bleProtocol_send("ERROR:STATE_LOCK"); return; }

  if (g_opState.state != IDLE) {
    opStateUnlock();
    bleProtocol_send("ERROR:BUSY");
    return;
  }

  g_opState.state = RUNNING;
  g_opState.currentCmdId = command.cmdId;
  g_opState.sessionId = command.sessionId;
  opStateUnlock();

  if (!valveController_startDispensacao(requestedMl, command.cmdId, command.sessionId)) {
    if (opStateLock()) {
      g_opState.state = IDLE;
      g_opState.currentCmdId = "";
      g_opState.sessionId = "";
      opStateUnlock();
    }
    bleProtocol_send("ERROR:BUSY");
    return;
  }

  cmdHistory_markPending(command.cmdId, command.sessionId);
  bleProtocol_send(String("ACK|") + command.cmdId);
  abrirValvula();

  Serial.printf("[SERVE] Iniciado: %u ml | CMD: %s | SESSION: %s\n",
                requestedMl, command.cmdId.c_str(), command.sessionId.c_str());
}

void handleStop(const ParsedCommand& command) {
  if (hasActiveSessionMismatch(command)) {
    Serial.printf("[SEC] Session mismatch CMD_ID=%s\n", command.cmdId.c_str());
    bleProtocol_send("ERROR:SESSION_MISMATCH");
    return;
  }
  Serial.printf("[STOP] Parada solicitada. CMD: %s\n", command.cmdId.c_str());
  valveController_stop(command.cmdId, command.sessionId);
}

}  // namespace

bool commandParser_parse(const String& rawCommand, ParsedCommand& parsed) {
  parsed = {};
  String text = rawCommand;
  text.trim();
  if (text.isEmpty()) return false;

  String parts[4];
  int partCount = 0;
  int from = 0;
  while (from <= (int)text.length() && partCount < 4) {
    const int sep = text.indexOf('|', from);
    if (sep < 0) { parts[partCount++] = text.substring(from); break; }
    parts[partCount++] = text.substring(from, sep);
    from = sep + 1;
  }

  if (partCount < 3) return false;

  parsed.name = parts[0];
  parsed.name.trim();
  parsed.name.toUpperCase();

  if (parsed.name == "AUTH" || parsed.name == "SERVE") {
    if (partCount != 4) return false;
    parsed.param = parts[1];
    parsed.cmdId = parts[2];
    parsed.sessionId = parts[3];
  } else if (parsed.name == "PING" || parsed.name == "STOP") {
    if (partCount != 3) return false;
    parsed.cmdId = parts[1];
    parsed.sessionId = parts[2];
  } else {
    return false;
  }

  parsed.param.trim();
  parsed.cmdId.trim();
  parsed.sessionId.trim();
  parsed.valid = !parsed.cmdId.isEmpty() && !parsed.sessionId.isEmpty();
  return parsed.valid;
}

void taskCommandProcessor(void* param) {
  (void)param;

  for (;;) {
    String rawCommand;
    if (!cmdQueue_dequeue(rawCommand, portMAX_DELAY)) continue;

    ParsedCommand command;
    if (!commandParser_parse(rawCommand, command)) {
      Serial.printf("[CMD] Formato inválido: %s\n", rawCommand.c_str());
      bleProtocol_send("ERROR:INVALID_FORMAT");
      continue;
    }

    if (command.name == "SERVE" && handleServeDuplicate(command)) continue;
    if (command.name == "AUTH") { handleAuth(command); continue; }
    if (command.name == "PING") { handlePing(command); continue; }
    if (!requireAuthenticatedSession()) continue;

    if (command.name == "SERVE") handleServe(command);
    else if (command.name == "STOP") handleStop(command);
    else bleProtocol_send("ERROR:UNKNOWN_COMMAND");
  }
}
