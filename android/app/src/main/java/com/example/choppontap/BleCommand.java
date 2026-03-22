package com.example.choppontap;

/**
 * BleCommand v2.0 — Constantes e utilitários do protocolo BLE CHOPP Franquia.
 *
 * PROTOCOLO v2.0 (compatível com firmware v2.0.0):
 *   AUTH  |<hmac_token>|<cmdId>|<sessionId>  → AUTH_OK|<cmdId>|<sessionId>
 *   SERVE |<ml>|<cmdId>|<sessionId>          → ACK|<cmdId> ... DONE|<cmdId>|<mlReal>|<sessionId>
 *   STOP  |<cmdId>|<sessionId>               → (sem resposta direta)
 *   PING  |<cmdId>|<sessionId>               → PONG|<cmdId>
 *
 * RESPOSTAS DE ERRO:
 *   ERROR:NOT_AUTHENTICATED    — Comando enviado antes do AUTH
 *   ERROR:INVALID_TOKEN        — Token HMAC invalido ou expirado
 *   ERROR:INVALID_AUTH         — Formato de AUTH invalido
 *   ERROR:SESSION_MISMATCH     — Session ID nao corresponde a sessao ativa
 *   ERROR:BUSY                 — Dispensacao ja em andamento
 *   ERROR:INVALID_ML           — Volume invalido (0 ou negativo)
 *   ERROR:VOLUME_EXCEEDED      — Volume acima de 2000ml
 *   ERROR:INVALID_FORMAT       — Comando malformado
 *   ERROR:UNKNOWN_COMMAND      — Comando desconhecido
 *   WARN:FLOW_TIMEOUT          — Barril vazio ou entupimento detectado
 *
 * CICLO DE VIDA DO COMANDO:
 *   QUEUED → SENT → ACKED → DONE (ou ERROR)
 */
public class BleCommand {

    // ── Tipos de comando ──────────────────────────────────────────────────
    public enum Type {
        AUTH,   // Autenticacao HMAC-SHA256
        SERVE,  // Liberacao de chopp
        STOP,   // Parar dispensacao
        PING    // Heartbeat
    }

    // ── Estados do ciclo de vida ──────────────────────────────────────────
    public enum State {
        QUEUED,   // Na fila, aguardando envio
        SENT,     // Enviado via BLE, aguardando ACK
        ACKED,    // ACK recebido, aguardando DONE
        DONE,     // DONE recebido — operacao concluida com sucesso
        ERROR     // Erro (timeout, BUSY, token invalido, etc.)
    }

    // ── Nomes dos comandos ────────────────────────────────────────────────
    public static final String CMD_AUTH  = "AUTH";
    public static final String CMD_SERVE = "SERVE";
    public static final String CMD_STOP  = "STOP";
    public static final String CMD_PING  = "PING";

    // ── Prefixos de resposta ──────────────────────────────────────────────
    public static final String RESP_AUTH_OK = "AUTH_OK";
    public static final String RESP_ACK     = "ACK";
    public static final String RESP_DONE    = "DONE";
    public static final String RESP_PONG    = "PONG";
    public static final String RESP_ERROR   = "ERROR";
    public static final String RESP_WARN    = "WARN";

    // ── Erros especificos ─────────────────────────────────────────────────
    public static final String ERR_NOT_AUTHENTICATED = "ERROR:NOT_AUTHENTICATED";
    public static final String ERR_INVALID_TOKEN     = "ERROR:INVALID_TOKEN";
    public static final String ERR_INVALID_AUTH      = "ERROR:INVALID_AUTH";
    public static final String ERR_SESSION_MISMATCH  = "ERROR:SESSION_MISMATCH";
    public static final String ERR_BUSY              = "ERROR:BUSY";
    public static final String ERR_INVALID_ML        = "ERROR:INVALID_ML";
    public static final String ERR_VOLUME_EXCEEDED   = "ERROR:VOLUME_EXCEEDED";
    public static final String ERR_INVALID_FORMAT    = "ERROR:INVALID_FORMAT";
    public static final String ERR_UNKNOWN_COMMAND   = "ERROR:UNKNOWN_COMMAND";
    public static final String WARN_FLOW_TIMEOUT     = "WARN:FLOW_TIMEOUT";

    // ── Limites de volume ─────────────────────────────────────────────────
    public static final int ML_MIN = 1;
    public static final int ML_MAX = 2000;

    // ── Volumes padrao (ml) ───────────────────────────────────────────────
    public static final int ML_COPO_PEQUENO = 200;
    public static final int ML_COPO_MEDIO   = 300;
    public static final int ML_COPO_GRANDE  = 500;
    public static final int ML_CANECA       = 700;
    public static final int ML_LITRO        = 1000;

    // ── Separador do protocolo ────────────────────────────────────────────
    public static final String SEPARATOR = "|";

    // ── Limite de tentativas ──────────────────────────────────────────────
    public static final int MAX_RETRIES = 3;

    // ── Campos da instancia ───────────────────────────────────────────────
    public final Type   type;
    public final String commandId;
    public final String sessionId;
    public final int    volumeMl;
    public final long   timestamp;

    public State  state        = State.QUEUED;
    public int    mlReal       = 0;
    public int    retryCount   = 0;
    public String errorMessage = null;

    // ── Construtor ────────────────────────────────────────────────────────
    public BleCommand(Type type, String commandId, String sessionId, int volumeMl) {
        this.type      = type;
        this.commandId = commandId;
        this.sessionId = sessionId;
        this.volumeMl  = volumeMl;
        this.timestamp = System.currentTimeMillis();
    }

    // =====================================================================
    // CONSTRUTORES DE COMANDOS (estaticos)
    // =====================================================================

    /**
     * Monta o comando SERVE.
     * Formato: SERVE|<ml>|<cmdId>|<sessionId>
     */
    public static String buildServe(int ml, String cmdId, String sessionId) {
        return CMD_SERVE + SEPARATOR + ml + SEPARATOR + cmdId + SEPARATOR + sessionId;
    }

    /**
     * Monta o comando STOP.
     * Formato: STOP|<cmdId>|<sessionId>
     */
    public static String buildStop(String cmdId, String sessionId) {
        return CMD_STOP + SEPARATOR + cmdId + SEPARATOR + sessionId;
    }

    /**
     * Monta o comando PING.
     * Formato: PING|<cmdId>|<sessionId>
     */
    public static String buildPing(String cmdId, String sessionId) {
        return CMD_PING + SEPARATOR + cmdId + SEPARATOR + sessionId;
    }

    /**
     * Gera a string BLE para esta instancia de comando.
     * Nota: AUTH deve ser gerado pelo BleManager (requer HMAC).
     */
    public String toBleString() {
        switch (type) {
            case SERVE:
                return buildServe(volumeMl, commandId, sessionId);
            case STOP:
                return buildStop(commandId, sessionId);
            case PING:
                return buildPing(commandId, sessionId);
            default:
                return CMD_PING + SEPARATOR + commandId + SEPARATOR + sessionId;
        }
    }

    // =====================================================================
    // PARSERS DE RESPOSTA
    // =====================================================================

    /**
     * Extrai o volume real (mlReal) de uma resposta DONE.
     * Formato esperado: DONE|<cmdId>|<mlReal>|<sessionId>
     *
     * @return Volume real dispensado em ml, ou -1 se invalido
     */
    public static int parseDoneMl(String response) {
        if (response == null || !response.startsWith(RESP_DONE + SEPARATOR)) return -1;
        String[] parts = response.split("\\" + SEPARATOR);
        if (parts.length < 3) return -1;
        try {
            return Integer.parseInt(parts[2].trim());
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /**
     * Extrai o CMD_ID de uma resposta (ACK, DONE, AUTH_OK, PONG).
     */
    public static String parseCmdId(String response) {
        if (response == null) return "";
        String[] parts = response.split("\\" + SEPARATOR);
        if (parts.length < 2) return "";
        return parts[1].trim();
    }

    public static boolean isError(String response) {
        return response != null && response.startsWith(RESP_ERROR);
    }

    public static boolean isWarning(String response) {
        return response != null && response.startsWith(RESP_WARN);
    }

    public static boolean isDone(String response) {
        return response != null && response.startsWith(RESP_DONE + SEPARATOR);
    }

    public static boolean isAuthOk(String response) {
        return response != null && response.startsWith(RESP_AUTH_OK + SEPARATOR);
    }

    public static boolean isAck(String response) {
        return response != null && response.startsWith(RESP_ACK + SEPARATOR);
    }

    public static boolean isPong(String response) {
        return response != null && response.startsWith(RESP_PONG + SEPARATOR);
    }

    public static boolean isFlowTimeout(String response) {
        return response != null && response.startsWith(WARN_FLOW_TIMEOUT);
    }

    public boolean canRetry() {
        return retryCount < MAX_RETRIES;
    }

    @Override
    public String toString() {
        return "BleCommand{"
                + "type=" + type
                + ", id=" + commandId
                + ", session=" + sessionId
                + ", vol=" + volumeMl + "ml"
                + ", state=" + state
                + ", retry=" + retryCount
                + "}";
    }
}
