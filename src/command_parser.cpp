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

bool requireReadyGuard(const ParsedCommand& command) {
  uint32_t readyAtMs = 0;
  bool ready = false;
  bool mtuUpdated = false;
  if (opStateLock()) {
    ready = g_opState.ready;
    mtuUpdated = g_opState.mtuAtualizado;
    readyAtMs = g_opState.readyAtMs;
    opStateUnlock();
  }

  if (!ready) {
    Serial.printf("[READY] Aguardando READY. CMD=%s SESSION=%s\n",
                  command.cmdId.c_str(), command.sessionId.c_str());
    bleProtocol_send("ERROR:NOT_READY");
    return false;
  }

  const uint32_t nowMs = millis();
  const uint32_t delta = nowMs - readyAtMs;
  Serial.printf("[READY] READY->SERVE: %lu ms | mtu=%s\n",
                static_cast<unsigned long>(delta),
                mtuUpdated ? "OK" : "NAO");

  if (delta < BLE_READY_GUARD_MS) {
    Serial.printf("[READY] Guard-band ativo (%lu ms < %lu ms)\n",
                  static_cast<unsigned long>(delta),
                  static_cast<unsigned long>(BLE_READY_GUARD_MS));
    bleProtocol_send("ERROR:READY_WAIT");
    return false;
  }

  return true;
}
// ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Handlers ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬

void handleAuth(const ParsedCommand& command) {
  if (command.param.length() < BLE_AUTH_TOKEN_MIN_LEN || command.sessionId.isEmpty()) {
    Serial.println("[AUTH] Formato invÃƒÆ’Ã‚Â¡lido");
    bleProtocol_send("ERROR:INVALID_AUTH");
    return;
  }

  // ValidaÃƒÆ’Ã‚Â§ÃƒÆ’Ã‚Â£o HMAC-SHA256 do token
  if (!authValidator_validate(command.param, command.sessionId)) {
    Serial.printf("[AUTH] Token invÃƒÆ’Ã‚Â¡lido: %s\n", authValidator_lastError());
    bleProtocol_send(String("ERROR:INVALID_TOKEN|") + authValidator_lastError());
    return;
  }

  if (!opStateLock()) { bleProtocol_send("ERROR:STATE_LOCK"); return; }
  g_opState.autenticado = true;
  g_opState.sessionId = command.sessionId;
  g_opState.currentCmdId = "";
  g_opState.state = IDLE;
  g_opState.ready = true;
  g_opState.readyAtMs = millis();
  opStateUnlock();

  Serial.printf("[AUTH] v%s Autenticado. Session: %s\\n", FW_VERSION, command.sessionId.c_str());\r\n  // v2.1.0: Android recebe versao do firmware no AUTH_OK para exibir no display\r\n  // Formato: AUTH_OK|<cmdId>|<sessionId>|FW:2.1.0|BUILD:Mar 29 2026\r\n  String authReply = makeReply("AUTH_OK", command.cmdId, command.sessionId);\r\n  authReply += String("|FW:") + FW_VERSION + "|BUILD:" + FW_BUILD_DATE;\r\n  bleProtocol_send(authReply);
}

void handlePing(const ParsedCommand& command) {
  (void)command.sessionId;
  bleProtocol_send(String("PONG|") + command.cmdId);
}

void handleReady(const ParsedCommand& command) {
  if (!isAuthenticated()) {
    bleProtocol_send("ERROR:NOT_AUTHENTICATED");
    return;
  }

  if (hasActiveSessionMismatch(command)) {
    Serial.printf("[SEC] Session mismatch CMD_ID=%s\n", command.cmdId.c_str());
    bleProtocol_send("ERROR:SESSION_MISMATCH");
    return;
  }

  uint32_t sinceConnect = 0;
  bool mtuUpdated = false;
  if (opStateLock()) {
    g_opState.ready = true;
    g_opState.readyAtMs = millis();
    mtuUpdated = g_opState.mtuAtualizado;
    if (g_opState.lastConnectMs > 0) {
      sinceConnect = g_opState.readyAtMs - g_opState.lastConnectMs;
    }
    opStateUnlock();
  }

  Serial.printf("[READY] OK | since_connect=%lu ms | mtu=%s\n",
                static_cast<unsigned long>(sinceConnect),
                mtuUpdated ? "OK" : "NAO");
  bleProtocol_send(makeReply("READY_OK", command.cmdId, command.sessionId));
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
    Serial.println("[DUP] Resultado reaproveitado do histÃƒÆ’Ã‚Â³rico persistente");
    bleProtocol_send(makeDoneReply(command.cmdId, prior.mlReal, String(prior.sessionId)));
    return true;
  }
  // Ainda em andamento — confirma ACK e re-registra sessão
  // para que finalizeDispense() encontre sessão válida e envie DONE
  bleProtocol_send(String("ACK|") + command.cmdId);
  if (opStateLock()) {
    g_opState.currentCmdId = command.cmdId;
    g_opState.sessionId    = command.sessionId;
    opStateUnlock();
  }
  Serial.printf("[DUP] Sessão re-registrada CMD=%s SESSION=%s\n",
                command.cmdId.c_str(), command.sessionId.c_str());
  return true;
}

void handleServe(const ParsedCommand& command) {
  if (hasActiveSessionMismatch(command)) {
    Serial.printf("[SEC] Session mismatch CMD_ID=%s\n", command.cmdId.c_str());
    bleProtocol_send("ERROR:SESSION_MISMATCH");
    return;
  }

  if (!requireReadyGuard(command)) {
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

    // v2.1.0 FIX: abrirValvula() ANTES de markPending.
  // taskDispensacao (prio 4) inicia imediatamente apos startDispensacao().
  // Valvula deve estar aberta antes do loop de fluxo comecar a contar pulsos.
  abrirValvula();
  cmdHistory_markPending(command.cmdId, command.sessionId);
  bleProtocol_send(String("ACK|") + command.cmdId);

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
  } else if (parsed.name == "PING" || parsed.name == "STOP" || parsed.name == "READY") {
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
      Serial.printf("[CMD] Formato invÃƒÆ’Ã‚Â¡lido: %s\n", rawCommand.c_str());
      bleProtocol_send("ERROR:INVALID_FORMAT");
      continue;
    }

    if (command.name == "SERVE" && handleServeDuplicate(command)) continue;
    if (command.name == "AUTH") { handleAuth(command); continue; }
    if (command.name == "READY") { handleReady(command); continue; }
    if (command.name == "PING") { handlePing(command); continue; }
    if (!requireAuthenticatedSession()) continue;

    if (command.name == "SERVE") handleServe(command);
    else if (command.name == "STOP") handleStop(command);
    else bleProtocol_send("ERROR:UNKNOWN_COMMAND");
  }
}













