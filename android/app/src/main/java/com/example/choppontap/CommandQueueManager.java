package com.example.choppontap;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.LinkedList;
import java.util.Queue;
import java.util.UUID;

/**
 * CommandQueueManager â€” Fila FIFO de comandos BLE Industrial v2.3.
 *
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * ARQUITETURA
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *
 * Fila FIFO: apenas 1 comando ativo por vez.
 * Fluxo por comando:
 *   enqueue() â†’ send â†’ aguarda ACK (5s) â†’ aguarda DONE (45s) â†’ remove â†’ prÃ³ximo
 *
 * DeduplicaÃ§Ã£o:
 *   O firmware ESP32 usa CMD_ID para detectar duplicatas.
 *   O CommandQueueManager mantÃ©m o mesmo CMD_ID em reenvios,
 *   garantindo que o ESP32 responda ACK sem executar novamente.
 *
 * ReconexÃ£o inteligente:
 *   Se BLE desconectar com comando SENT/ACKED em andamento,
 *   ao reconectar o mesmo comando Ã© reenviado com o mesmo ID.
 *   O ESP32 responde ACK (se ainda executando) ou DONE (se jÃ¡ terminou).
 *
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * CALLBACKS
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *
 *   onSend(cmd)    â€” chamado quando o comando Ã© enviado via BLE
 *   onAck(cmd)     â€” chamado quando ACK|ID Ã© recebido
 *   onDone(cmd)    â€” chamado quando DONE|ID|ml Ã© recebido
 *   onError(cmd)   â€” chamado em timeout ou erro irrecuperÃ¡vel
 */
public class CommandQueueManager {

    private static final String TAG = "BLE_CMD_QUEUE";

    // â”€â”€ Timeouts â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    /** Timeout para receber ACK apÃ³s envio (ms). Firmware garante ACK em < 100ms. */
    private static final long ACK_TIMEOUT_MS  = 5_000L;
    /** Timeout para receber DONE apÃ³s ACK (ms). OperaÃ§Ã£o mÃ¡xima: 10s no firmware. */
    private static final long DONE_TIMEOUT_MS = 45_000L;

    // â”€â”€ Estado interno â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    private final Queue<BleCommand> mQueue   = new LinkedList<>();
    private BleCommand              mActive  = null;  // Comando em andamento
    private boolean                 mPaused  = false; // Pausa durante desconexÃ£o BLE

    private final Handler  mHandler = new Handler(Looper.getMainLooper());
    private Runnable       mAckTimeoutRunnable  = null;
    private Runnable       mDoneTimeoutRunnable = null;

    // â”€â”€ Callbacks â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    public interface Callback {
        void onSend(BleCommand cmd);
        void onAck(BleCommand cmd);
        void onDone(BleCommand cmd);
        void onError(BleCommand cmd, String reason);
    }

    private Callback mCallback;

    // â”€â”€ Interface de envio BLE (injetada pelo BluetoothService) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    public interface BleWriter {
        /** Envia a string via BLE. Retorna true se enviado com sucesso. */
        boolean write(String data);
    }

    private BleWriter mWriter;

    // â”€â”€ Construtor â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    public CommandQueueManager(BleWriter writer, Callback callback) {
        this.mWriter   = writer;
        this.mCallback = callback;
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // API pÃºblica
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    /**
     * Gera um novo BleCommand de SERVE com IDs Ãºnicos e o enfileira.
     *
     * @param volumeMl Volume em ml a ser liberado
     * @return O BleCommand criado (para rastreamento pelo chamador)
     */
    public synchronized BleCommand enqueueServe(int volumeMl) {
        String cmdId    = UUID.randomUUID().toString().replace("-", "").substring(0, 8).toUpperCase();
        String sessionId = "SES_" + UUID.randomUUID().toString().replace("-", "").substring(0, 8).toUpperCase();
        BleCommand cmd = new BleCommand(BleCommand.Type.SERVE, cmdId, sessionId, volumeMl);
        mQueue.add(cmd);
        Log.i(TAG, "[BLE_CMD] enqueue â†’ " + cmd);
        processQueue();
        return cmd;
    }

    /**
     * Enfileira um comando PING para heartbeat.
     */
    public synchronized void enqueuePing() {
        String cmdId = UUID.randomUUID().toString().replace("-", "").substring(0, 4).toUpperCase();
        BleCommand cmd = new BleCommand(BleCommand.Type.PING, cmdId, "", 0);
        mQueue.add(cmd);
        processQueue();
    }

    /**
     * Processa a resposta BLE recebida do ESP32.
     * Deve ser chamado pelo BluetoothService em onCharacteristicChanged().
     *
     * Formatos tratados:
     *   ACK|<id>
     *   DONE|<id>|<ml>
     *   DONE|<id>|<ml>|<session>
     *   DONE (sem ID â€” legado)
     *   DUPLICATE / ML:DUPLICATE
     *   ERROR:BUSY
     *   ERROR:WATCHDOG
     *   PONG
     */
    public synchronized void onBleResponse(String response) {
        if (response == null || response.isEmpty()) return;

        Log.d(TAG, "[BLE_CMD] resposta recebida: [" + response + "] | ativo=" + mActive);

        // â”€â”€ ML:ACK (protocolo real ESP32) ou ACK|<id> (formato v2.3 doc) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.equalsIgnoreCase("ML:ACK") || response.startsWith("ACK|")) {
            String ackId = response.startsWith("ACK|") ? response.substring(4).trim() : null;
            if (mActive != null) {
                // Se o ID estÃ¡ presente, valida; se nÃ£o, aceita para o comando ativo
                boolean idMatch = (ackId == null || ackId.isEmpty()
                        || mActive.commandId.equalsIgnoreCase(ackId));
                if (idMatch) {
                    Log.i(TAG, "[BLE_CMD] ack â†’ " + mActive.commandId);
                    cancelAckTimeout();
                    mActive.state = BleCommand.State.ACKED;
                    if (mCallback != null) mCallback.onAck(mActive);
                    iniciarDoneTimeout();
                } else {
                    Log.w(TAG, "[BLE_CMD] ACK ignorado â€” id=" + ackId + " | ativo=" + mActive.commandId);
                }
            } else {
                Log.w(TAG, "[BLE_CMD] ACK recebido sem comando ativo");
            }
            return;
        }

        // â”€â”€ DONE com ou sem ID â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.startsWith("DONE")) {
            handleDone(response);
            return;
        }

        // â”€â”€ DUPLICATE / ML:DUPLICATE â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.equalsIgnoreCase("DUPLICATE") || response.equalsIgnoreCase("ML:DUPLICATE")) {
            Log.w(TAG, "[BLE_CMD] DUPLICATE recebido â€” sincronizando estado");
            if (mActive != null) {
                // O firmware jÃ¡ executou este comando â€” tratar como DONE sem ml_real
                mActive.state = BleCommand.State.DONE;
                cancelAllTimeouts();
                if (mCallback != null) mCallback.onDone(mActive);
                mActive = null;
                processQueue();
            }
            return;
        }

        // â”€â”€ ERROR:BUSY â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.equalsIgnoreCase("ERROR:BUSY")) {
            Log.w(TAG, "[BLE_CMD] ERROR:BUSY â€” ESP32 ocupado, aguardando 2s para reenvio");
            if (mActive != null && mActive.canRetry()) {
                mActive.retryCount++;
                mActive.state = BleCommand.State.QUEUED;
                cancelAllTimeouts();
                mHandler.postDelayed(this::processQueue, 2_000L);
            } else if (mActive != null) {
                falharComando(mActive, "ERROR:BUSY â€” mÃ¡ximo de retries atingido");
            }
            return;
        }

        // â”€â”€ ERROR:WATCHDOG â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.startsWith("ERROR:WATCHDOG")) {
            Log.e(TAG, "[BLE_CMD] ERROR:WATCHDOG recebido do ESP32");
            if (mActive != null) {
                falharComando(mActive, "ERROR:WATCHDOG");
            }
            return;
        }

        // â”€â”€ PONG â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (response.equalsIgnoreCase("PONG")) {
            Log.d(TAG, "[BLE_CMD] PONG recebido â€” BLE ativo");
            // Remove PING da fila se for o ativo
            if (mActive != null && mActive.type == BleCommand.Type.PING) {
                cancelAllTimeouts();
                mActive.state = BleCommand.State.DONE;
                mActive = null;
                processQueue();
            }
        }
    }

    /**
     * Chamado pelo BluetoothService quando o BLE desconecta.
     * Pausa a fila â€” o comando ativo permanece para reenvio apÃ³s reconexÃ£o.
     */
    public synchronized void onBleDisconnected() {
        Log.w(TAG, "[BLE_CMD] BLE desconectado â€” pausando fila | ativo=" + mActive);
        mPaused = true;
        cancelAllTimeouts();
        if (mActive != null &&
                (mActive.state == BleCommand.State.SENT ||
                 mActive.state == BleCommand.State.ACKED)) {
            mActive.state = BleCommand.State.QUEUED;
            Log.i(TAG, "[BLE_CMD] retry â†’ " + mActive.commandId
                    + " state=" + mActive.state
                    + " (mesmo ID para deduplicaÃ§Ã£o no ESP32)");
        }
    }

    /**
     * Chamado pelo BluetoothService quando o BLE reconecta e estÃ¡ READY.
     * Retoma a fila â€” reenvio do comando ativo com mesmo ID.
     */
    public synchronized void onBleReady() {
        Log.i(TAG, "[BLE_CMD] BLE READY â€” retomando fila | ativo=" + mActive + " | fila=" + mQueue.size());
        mPaused = false;
        processQueue();
    }

    /**
     * Retorna o comando SERVE ativo (se houver), para consulta pelo PagamentoConcluido.
     */
    public synchronized BleCommand getActiveCommand() {
        return mActive;
    }

    /**
     * Limpa toda a fila e cancela o comando ativo.
     * Usar apenas em reset de emergÃªncia.
     */
    public synchronized void reset() {
        Log.w(TAG, "[BLE_CMD] reset() â€” limpando fila e cancelando ativo");
        cancelAllTimeouts();
        mQueue.clear();
        mActive = null;
        mPaused = false;
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Processamento interno
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    private void processQueue() {
        if (mPaused) {
            Log.d(TAG, "[BLE_CMD] processQueue() â€” PAUSADO (BLE desconectado)");
            return;
        }
        if (mActive != null) {
            Log.d(TAG, "[BLE_CMD] processQueue() â€” aguardando conclusÃ£o de " + mActive.commandId);
            return;
        }
        if (mQueue.isEmpty()) {
            Log.d(TAG, "[BLE_CMD] processQueue() â€” fila vazia");
            return;
        }

        mActive = mQueue.poll();
        enviarComandoAtivo();
    }

    private void enviarComandoAtivo() {
        if (mActive == null) return;

        String bleStr = mActive.toBleString();
        Log.i(TAG, "[BLE_CMD] sent â†’ " + mActive.commandId + " | cmd=[" + bleStr + "]");

        boolean ok = mWriter.write(bleStr);
        if (ok) {
            mActive.state = BleCommand.State.SENT;
            if (mCallback != null) mCallback.onSend(mActive);
            // PINGs nÃ£o precisam de ACK/DONE â€” timeout curto
            if (mActive.type == BleCommand.Type.PING) {
                iniciarAckTimeout(3_000L);
            } else {
                iniciarAckTimeout(ACK_TIMEOUT_MS);
            }
        } else {
            Log.e(TAG, "[BLE_CMD] write() falhou para " + mActive.commandId + " â€” agendando retry");
            mActive.retryCount++;
            if (mActive.canRetry()) {
                mActive.state = BleCommand.State.QUEUED;
                mQueue.add(mActive); // Recoloca no final da fila
                mActive = null;
                mHandler.postDelayed(this::processQueue, 1_000L);
            } else {
                falharComando(mActive, "write() falhou apÃ³s " + BleCommand.MAX_RETRIES + " tentativas");
            }
        }
    }

    private void handleDone(String response) {
        // Formatos:
        //   DONE
        //   DONE|<id>|<ml>
        //   DONE|<id>|<ml>|<session>
        String[] parts = response.split("\\|");
        String doneId = parts.length >= 2 ? parts[1].trim() : null;
        int mlReal = 0;
        if (parts.length >= 3) {
            try { mlReal = Integer.parseInt(parts[2].trim()); } catch (Exception ignored) {}
        }

        if (mActive == null) {
            Log.w(TAG, "[BLE_CMD] DONE recebido sem comando ativo â€” ignorando");
            return;
        }

        // Verifica se o ID bate (se houver ID no DONE)
        if (doneId != null && !doneId.isEmpty() && !doneId.equalsIgnoreCase(mActive.commandId)) {
            Log.w(TAG, "[BLE_CMD] DONE id=" + doneId + " nÃ£o bate com ativo=" + mActive.commandId + " â€” ignorando");
            return;
        }

        Log.i(TAG, "[BLE_CMD] done â†’ " + mActive.commandId + " | ml_real=" + mlReal);
        cancelAllTimeouts();
        mActive.state  = BleCommand.State.DONE;
        mActive.mlReal = mlReal;
        if (mCallback != null) mCallback.onDone(mActive);
        mActive = null;
        processQueue();
    }

    private void falharComando(BleCommand cmd, String reason) {
        Log.e(TAG, "[BLE_CMD] error â†’ " + cmd.commandId + " | motivo=" + reason);
        cancelAllTimeouts();
        cmd.state        = BleCommand.State.ERROR;
        cmd.errorMessage = reason;
        if (mCallback != null) mCallback.onError(cmd, reason);
        mActive = null;
        mQueue.clear(); // Limpa fila em caso de erro irrecuperÃ¡vel
    }

    // â”€â”€ Timeouts â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    private void iniciarAckTimeout(long ms) {
        cancelAckTimeout();
        mAckTimeoutRunnable = () -> {
            synchronized (CommandQueueManager.this) {
                if (mActive == null || mActive.state != BleCommand.State.SENT) return;
                Log.e(TAG, "[BLE_CMD] ACK TIMEOUT (" + ms + "ms) para " + mActive.commandId
                        + " | retry=" + mActive.retryCount);
                if (mActive.canRetry()) {
                    mActive.retryCount++;
                    mActive.state = BleCommand.State.QUEUED;
                    Log.i(TAG, "[BLE_CMD] retry â†’ " + mActive.commandId
                            + " (tentativa " + mActive.retryCount + "/" + BleCommand.MAX_RETRIES + ")");
                    enviarComandoAtivo();
                } else {
                    falharComando(mActive, "ACK timeout apÃ³s " + BleCommand.MAX_RETRIES + " tentativas");
                }
            }
        };
        mHandler.postDelayed(mAckTimeoutRunnable, ms);
    }

    private void iniciarDoneTimeout() {
        cancelDoneTimeout();
        mDoneTimeoutRunnable = () -> {
            synchronized (CommandQueueManager.this) {
                if (mActive == null || mActive.state != BleCommand.State.ACKED) return;
                Log.e(TAG, "[BLE_CMD] DONE TIMEOUT (" + DONE_TIMEOUT_MS + "ms) para " + mActive.commandId);
                falharComando(mActive, "DONE timeout apÃ³s " + DONE_TIMEOUT_MS + "ms");
            }
        };
        mHandler.postDelayed(mDoneTimeoutRunnable, DONE_TIMEOUT_MS);
    }

    private void cancelAckTimeout() {
        if (mAckTimeoutRunnable != null) {
            mHandler.removeCallbacks(mAckTimeoutRunnable);
            mAckTimeoutRunnable = null;
        }
    }

    private void cancelDoneTimeout() {
        if (mDoneTimeoutRunnable != null) {
            mHandler.removeCallbacks(mDoneTimeoutRunnable);
            mDoneTimeoutRunnable = null;
        }
    }

    private void cancelAllTimeouts() {
        cancelAckTimeout();
        cancelDoneTimeout();
    }
}

