package com.chopp.app.ble

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.UUID
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * BleManager v2.0 — Gerenciador BLE para dispositivos CHOPP Franquia.
 *
 * PROTOCOLO v2.0 (compatível com firmware v2.0.0):
 *  - UUIDs: Serviço 7f0a0001-..., RX 7f0a0002-..., TX 7f0a0003-...
 *  - Autenticação HMAC-SHA256:
 *      token = HMAC-SHA256(sessionId + ":" + timestamp, AUTH_SECRET_KEY)
 *      formato do token: hmac_hex_64chars:timestamp_segundos
 *      comando: AUTH|<token>|<cmdId>|<sessionId>
 *  - Dispensação: SERVE|<ml>|<cmdId>|<sessionId>
 *  - Heartbeat:   PING|<cmdId>|<sessionId>
 *  - Parada:      STOP|<cmdId>|<sessionId>
 *
 * FLUXO DE ESTADOS:
 *  DESCONECTADO → CONECTADO → AUTENTICADO → PRONTO
 *
 * SEGURANÇA:
 *  - Token HMAC-SHA256 com janela de 5 minutos
 *  - Vinculação por MAC da torneira (via getMacSalvoDaApi)
 *  - CMD_ID único por comando para idempotência
 *  - SESSION_ID único por sessão de venda
 */
class BleManager(private val context: Context) {

    companion object {
        private const val TAG = "BleManager"

        // Prefixo obrigatório para aceitar dispositivo no scan
        private const val DEVICE_PREFIX = "CHOPP_"

        // ── UUIDs do Firmware v2.0.0 ──────────────────────────────────
        // ATENÇÃO: estes UUIDs devem ser idênticos ao config.h do firmware
        private val SERVICE_UUID           = UUID.fromString("7f0a0001-7b6b-4b5f-9d3e-3c7b9f100001")
        private val CHARACTERISTIC_UUID_RX = UUID.fromString("7f0a0002-7b6b-4b5f-9d3e-3c7b9f100001")
        private val CHARACTERISTIC_UUID_TX = UUID.fromString("7f0a0003-7b6b-4b5f-9d3e-3c7b9f100001")
        private val DESCRIPTOR_UUID        = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        // Chave secreta HMAC — deve ser idêntica ao AUTH_SECRET_KEY do firmware
        // IMPORTANTE: em produção, obter esta chave via backend seguro (ex: HTTPS + certificado)
        private const val AUTH_SECRET_KEY = "CHOPP_FRANQUIA_SECRET_2024_v2"

        // Tempo máximo de scan
        private const val SCAN_PERIOD_MS = 10_000L
    }

    // ── Estado de autenticação ────────────────────────────────────────────
    private enum class AuthState {
        DISCONNECTED, CONNECTED, AUTHENTICATED, READY
    }

    private var authState: AuthState = AuthState.DISCONNECTED

    // ── Infraestrutura BLE ────────────────────────────────────────────────
    private val bluetoothManager: BluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter = bluetoothManager.adapter
    private val bleScanner: BluetoothLeScanner = bluetoothAdapter.bluetoothLeScanner

    private var mBluetoothGatt: BluetoothGatt? = null
    private var mScanning: Boolean = false
    private val handler = Handler(Looper.getMainLooper())

    // ── Callbacks públicos ────────────────────────────────────────────────
    var onDeviceFound: ((BluetoothDevice) -> Unit)? = null
    var onConnected: (() -> Unit)? = null
    var onAuthenticated: (() -> Unit)? = null
    var onReady: (() -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onDataReceived: ((String) -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    // ── Sessão atual ──────────────────────────────────────────────────────
    private var currentSessionId: String = ""

    // ── MAC vinculado (via API) ───────────────────────────────────────────
    /**
     * Retorna o MAC da torneira vinculada a este usuário/franquia.
     * INTEGRAÇÃO: substituir pelo repositório/API real.
     */
    private fun getMacSalvoDaApi(): String? {
        // TODO: integrar com repositório/API real
        return null
    }

    // =====================================================================
    // GERAÇÃO DE TOKEN HMAC-SHA256
    // =====================================================================

    /**
     * Gera um token de autenticação HMAC-SHA256.
     *
     * Formato do token: hmac_hex_64chars:timestamp_segundos
     * Mensagem assinada: sessionId + ":" + timestamp_segundos
     *
     * @param sessionId SESSION_ID da sessão atual
     * @return Token no formato "hmac:timestamp" ou null em caso de erro
     */
    fun generateAuthToken(sessionId: String): String? {
        return try {
            val timestampSeconds = System.currentTimeMillis() / 1000L
            val message = "$sessionId:$timestampSeconds"

            val mac = Mac.getInstance("HmacSHA256")
            val keySpec = SecretKeySpec(AUTH_SECRET_KEY.toByteArray(Charsets.UTF_8), "HmacSHA256")
            mac.init(keySpec)

            val hmacBytes = mac.doFinal(message.toByteArray(Charsets.UTF_8))
            val hmacHex = hmacBytes.joinToString("") { "%02x".format(it) }

            val token = "$hmacHex:$timestampSeconds"
            Log.d(TAG, "[AUTH] Token HMAC gerado. Session: $sessionId, TS: $timestampSeconds")
            token
        } catch (e: Exception) {
            Log.e(TAG, "[AUTH] Erro ao gerar token HMAC: ${e.message}")
            null
        }
    }

    /**
     * Gera um ID único de 8 caracteres hexadecimais para CMD_ID ou SESSION_ID.
     */
    fun generateUniqueId(prefix: String = ""): String {
        val ts = System.currentTimeMillis().toString(16).takeLast(6).uppercase()
        val rnd = (Math.random() * 0xFFFF).toInt().toString(16).padStart(4, '0').uppercase()
        return if (prefix.isNotEmpty()) "${prefix}_${ts}${rnd}" else "${ts}${rnd}"
    }

    // =====================================================================
    // SCAN BLE
    // =====================================================================

    fun startScan() {
        if (mScanning) {
            Log.d(TAG, "[SCAN] Scan já em andamento.")
            return
        }
        Log.d(TAG, "[SCAN] Iniciando scan — prefixo: $DEVICE_PREFIX")
        mScanning = true
        handler.postDelayed({ stopScan() }, SCAN_PERIOD_MS)
        bleScanner.startScan(bleScanCallback)
    }

    fun stopScan() {
        if (!mScanning) return
        Log.d(TAG, "[SCAN] Parando scan.")
        mScanning = false
        bleScanner.stopScan(bleScanCallback)
    }

    private val bleScanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            super.onScanResult(callbackType, result)
            if (!isDeviceAceito(result)) return
            Log.d(TAG, "[SCAN] Aceito: ${result.device.name} | ${result.device.address}")
            broadcastDeviceFound(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            super.onScanFailed(errorCode)
            Log.e(TAG, "[SCAN] Falha. Código: $errorCode")
            mScanning = false
            onError?.invoke("Falha no scan BLE. Código: $errorCode")
        }
    }

    private fun isDeviceAceito(result: ScanResult): Boolean {
        val deviceName = result.device.name ?: return false
        if (!deviceName.startsWith(DEVICE_PREFIX)) return false

        val macSalvo = getMacSalvoDaApi()
        if (macSalvo != null && !result.device.address.equals(macSalvo, ignoreCase = true)) {
            Log.d(TAG, "[SCAN] MAC não corresponde. Encontrado: ${result.device.address} | Esperado: $macSalvo")
            return false
        }
        return true
    }

    private fun broadcastDeviceFound(device: BluetoothDevice) {
        onDeviceFound?.invoke(device)
    }

    // =====================================================================
    // CONEXÃO GATT
    // =====================================================================

    fun connect(device: BluetoothDevice) {
        Log.d(TAG, "[GATT] Conectando: ${device.name} (${device.address})")
        stopScan()
        mBluetoothGatt = device.connectGatt(context, false, gattCallback)
    }

    fun disconnect() {
        Log.d(TAG, "[GATT] Desconectando.")
        authState = AuthState.DISCONNECTED
        currentSessionId = ""
        mBluetoothGatt?.disconnect()
        mBluetoothGatt?.close()
        mBluetoothGatt = null
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothGatt.STATE_CONNECTED -> {
                    Log.d(TAG, "[GATT] Conectado. Descobrindo serviços...")
                    authState = AuthState.CONNECTED
                    gatt.discoverServices()
                    onConnected?.invoke()
                }
                BluetoothGatt.STATE_DISCONNECTED -> {
                    Log.d(TAG, "[GATT] Desconectado. Status: $status")
                    authState = AuthState.DISCONNECTED
                    currentSessionId = ""
                    mBluetoothGatt?.close()
                    mBluetoothGatt = null
                    onDisconnected?.invoke()
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e(TAG, "[GATT] Falha ao descobrir serviços. Status: $status")
                onError?.invoke("Falha ao descobrir serviços GATT. Status: $status")
                return
            }

            // Verificar se o serviço CHOPP v2.0 está presente
            val service = gatt.getService(SERVICE_UUID)
            if (service == null) {
                Log.e(TAG, "[GATT] Servico CHOPP v2.0 nao encontrado! UUID: $SERVICE_UUID")
                onError?.invoke("Dispositivo incompativel: firmware desatualizado ou UUID incorreto.")
                return
            }

            Log.d(TAG, "[GATT] Servico CHOPP v2.0 encontrado. Habilitando notificacoes TX...")
            enableTxNotifications(gatt)
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == CHARACTERISTIC_UUID_TX) {
                val data = characteristic.value?.toString(Charsets.UTF_8)?.trim() ?: return
                Log.d(TAG, "[GATT] RX: $data")
                handleReceivedData(data)
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "[GATT] Notificacoes TX habilitadas. Enviando AUTH HMAC...")
                sendAuthCommand()
            } else {
                Log.e(TAG, "[GATT] Falha ao habilitar notificacoes. Status: $status")
                onError?.invoke("Falha ao habilitar notificacoes GATT. Status: $status")
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.d(TAG, "[GATT] Escrita confirmada: ${characteristic.uuid}")
            } else {
                Log.e(TAG, "[GATT] Falha na escrita. Status: $status")
                onError?.invoke("Falha ao escrever na caracteristica GATT. Status: $status")
            }
        }
    }

    // =====================================================================
    // AUTENTICAÇÃO HMAC-SHA256
    // =====================================================================

    /**
     * Envia o comando AUTH com token HMAC-SHA256.
     * Chamado automaticamente após habilitar notificações TX.
     *
     * Formato: AUTH|<hmac_hex:timestamp>|<cmdId>|<sessionId>
     */
    private fun sendAuthCommand() {
        currentSessionId = generateUniqueId("SES")
        val cmdId = generateUniqueId("AUTH")

        val token = generateAuthToken(currentSessionId)
        if (token == null) {
            Log.e(TAG, "[AUTH] Falha ao gerar token HMAC")
            onError?.invoke("Falha ao gerar token de autenticacao.")
            return
        }

        val authCommand = "AUTH|$token|$cmdId|$currentSessionId"
        Log.d(TAG, "[AUTH] Enviando AUTH. Session: $currentSessionId")
        sendRawCommand(authCommand)
    }

    /**
     * Processa dados recebidos do ESP32.
     */
    private fun handleReceivedData(data: String) {
        when {
            data.startsWith("AUTH_OK|") -> {
                Log.d(TAG, "[AUTH] Autenticacao confirmada pelo ESP32.")
                authState = AuthState.AUTHENTICATED
                onAuthenticated?.invoke()
                authState = AuthState.READY
                onReady?.invoke()
            }
            data.startsWith("ERROR:INVALID_TOKEN") -> {
                Log.e(TAG, "[AUTH] Token HMAC rejeitado: $data")
                onError?.invoke("Autenticacao falhou: token invalido. Verifique a chave secreta.")
                disconnect()
            }
            data.startsWith("ERROR:INVALID_AUTH") -> {
                Log.e(TAG, "[AUTH] Formato de autenticacao invalido.")
                onError?.invoke("Erro de autenticacao: formato invalido.")
                disconnect()
            }
            data.startsWith("WARN:FLOW_TIMEOUT|") -> {
                Log.w(TAG, "[FLOW] Timeout de fluxo — barril possivelmente vazio!")
                onDataReceived?.invoke(data)
            }
            else -> {
                onDataReceived?.invoke(data)
            }
        }
    }

    // =====================================================================
    // ENVIO DE COMANDOS
    // =====================================================================

    /**
     * Envia um comando SERVE para dispensar chopp.
     *
     * @param ml Volume em mililitros (1 a 2000)
     * @param cmdId ID único do comando (use generateUniqueId())
     * @return true se o comando foi enviado com sucesso
     */
    fun sendServe(ml: Int, cmdId: String): Boolean {
        if (authState != AuthState.READY) {
            Log.w(TAG, "[CMD] SERVE bloqueado — estado: $authState")
            onError?.invoke("Aguarde a autenticacao antes de dispensar.")
            return false
        }
        if (ml <= 0 || ml > 2000) {
            onError?.invoke("Volume invalido: $ml ml (deve ser entre 1 e 2000)")
            return false
        }
        val command = "SERVE|$ml|$cmdId|$currentSessionId"
        Log.d(TAG, "[CMD] SERVE: $ml ml | CMD: $cmdId | SESSION: $currentSessionId")
        return sendRawCommand(command)
    }

    /**
     * Envia um comando STOP para parar a dispensação.
     *
     * @param cmdId ID único do comando STOP
     */
    fun sendStop(cmdId: String): Boolean {
        if (authState != AuthState.READY) {
            onError?.invoke("Aguarde a autenticacao antes de parar.")
            return false
        }
        val command = "STOP|$cmdId|$currentSessionId"
        Log.d(TAG, "[CMD] STOP | CMD: $cmdId")
        return sendRawCommand(command)
    }

    /**
     * Envia um PING para verificar conectividade.
     *
     * @param cmdId ID único do comando PING
     */
    fun sendPing(cmdId: String): Boolean {
        val command = "PING|$cmdId|$currentSessionId"
        Log.d(TAG, "[CMD] PING | CMD: $cmdId")
        return sendRawCommand(command)
    }

    /**
     * Retorna o SESSION_ID da sessão atual.
     */
    fun getCurrentSessionId(): String = currentSessionId

    /**
     * Retorna true se o dispositivo está pronto para receber comandos.
     */
    fun isReady(): Boolean = authState == AuthState.READY

    /**
     * Envia um comando raw via característica RX.
     * Uso interno — prefira os métodos sendServe, sendStop, sendPing.
     */
    fun sendRawCommand(command: String): Boolean {
        val gatt = mBluetoothGatt ?: run {
            Log.e(TAG, "[CMD] Sem conexao GATT ativa.")
            onError?.invoke("Sem conexao BLE ativa.")
            return false
        }

        val service = gatt.getService(SERVICE_UUID) ?: run {
            Log.e(TAG, "[CMD] Servico BLE nao encontrado: $SERVICE_UUID")
            onError?.invoke("Servico BLE nao encontrado.")
            return false
        }

        val rxCharacteristic = service.getCharacteristic(CHARACTERISTIC_UUID_RX) ?: run {
            Log.e(TAG, "[CMD] Caracteristica RX nao encontrada: $CHARACTERISTIC_UUID_RX")
            onError?.invoke("Caracteristica RX nao encontrada.")
            return false
        }

        rxCharacteristic.value = command.toByteArray(Charsets.UTF_8)
        rxCharacteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

        val success = gatt.writeCharacteristic(rxCharacteristic)
        Log.d(TAG, "[CMD] TX: $command | OK: $success")

        if (!success) onError?.invoke("Falha ao enviar: $command")
        return success
    }

    // =====================================================================
    // UTILITÁRIOS INTERNOS
    // =====================================================================

    private fun enableTxNotifications(gatt: BluetoothGatt) {
        val service = gatt.getService(SERVICE_UUID) ?: run {
            Log.e(TAG, "[GATT] Servico nao encontrado ao habilitar notificacoes.")
            return
        }
        val txCharacteristic = service.getCharacteristic(CHARACTERISTIC_UUID_TX) ?: run {
            Log.e(TAG, "[GATT] Caracteristica TX nao encontrada.")
            return
        }

        gatt.setCharacteristicNotification(txCharacteristic, true)
        val descriptor = txCharacteristic.getDescriptor(DESCRIPTOR_UUID)
        descriptor?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        gatt.writeDescriptor(descriptor)
    }
}
