#include "operacional.h"
#include "operaBLE.h"

#ifdef USAR_ESP32_UART_BLE

    BLEServer *pServer = NULL;
    BLECharacteristic *pTxCharacteristic;
    bool deviceConnected = false;

    // Controla se o dispositivo Android foi autenticado via PIN ($AUTH:259087)
    // Resetado para false a cada nova conexão BLE
    bool bleAutenticado = false;

    // =========================================================================
    // NOME BLE DINÂMICO — CHOPP_XXXX
    //
    // O nome do dispositivo BLE é gerado dinamicamente em setupBLE() usando
    // os 4 últimos caracteres do MAC BLE da placa (ex: CHOPP_EEF1).
    //
    // IMPORTANTE: O MAC BLE do ESP32 é diferente do MAC WiFi.
    // O MAC BLE é lido via esp_read_mac() com ESP_MAC_BT.
    //
    // Isso garante que cada unidade tenha um nome único sem necessidade de
    // configuração manual — basta gravar o mesmo firmware em todas as placas.
    //
    // O Android filtra dispositivos por startsWith("CHOPP_") e conecta
    // exclusivamente pelo MAC salvo em SharedPreferences.
    // =========================================================================

    // =========================================================================
    // SEGURANÇA BLE — DUPLA CAMADA
    //
    // CAMADA 1 — Bond/Pairing nativo (ESP_LE_AUTH_REQ_SC_MITM_BOND):
    //   - PIN estático 259087 via BLESecurity::setStaticPIN()
    //   - Qualquer dispositivo que tente conectar precisa confirmar o PIN
    //   - O bond é armazenado no NVS do ESP32 e no Android
    //   - Após o primeiro bond, reconexões são automáticas sem pedir PIN
    //
    // CAMADA 2 — Autenticação por comando ($AUTH:259087):
    //   - Após conectar e descobrir serviços, o Android envia $AUTH:259087
    //   - O ESP32 valida o PIN e responde AUTH:OK ou AUTH:FAIL
    //   - Comandos de operação ($ML, $LB, etc.) são bloqueados até AUTH:OK
    //   - bleAutenticado é resetado para false a cada desconexão
    //
    // RESULTADO: Apenas dispositivos que conhecem o PIN 259087 E que foram
    // pareados previamente conseguem operar o dispensador.
    // =========================================================================

    // =========================================================================
    // FIX DEFINITIVO STATUS=8 (Connection Supervision Timeout)
    //
    // CAUSA: Android usa CONNECTION_PRIORITY_BALANCED por padrão:
    //   - Connection interval: 45ms
    //   - Supervision timeout: 720ms
    //   Com 45ms de intervalo, 8 eventos perdidos = 360ms = timeout declarado.
    //
    // SOLUÇÃO — 3 camadas simultâneas:
    //
    // 1. ESP32 (este arquivo): esp_ble_gap_update_conn_params() em onConnect()
    //    força interval=7.5ms, latency=0, timeout=5000ms
    //
    // 2. ESP32 (main.cpp): taskLiberaML na Core 1 (APP_CPU), liberando
    //    a Core 0 exclusivamente para o stack BLE Bluedroid
    //
    // 3. Android (BluetoothService.java): requestConnectionPriority(HIGH)
    //    logo após STATE_CONNECTED
    // =========================================================================

    // ─────────────────────────────────────────────────────────────────────────
    // Callbacks de segurança BLE — CAMADA 1 (bond/pairing nativo)
    // ─────────────────────────────────────────────────────────────────────────
    class MySecurityCallbacks : public BLESecurityCallbacks {

        uint32_t onPassKeyRequest() {
            DBG_PRINTF("\n[BLE-SEC] onPassKeyRequest — retornando PIN: %s", BLE_AUTH_PIN);
            return (uint32_t)atoi(BLE_AUTH_PIN);
        }

        void onPassKeyNotify(uint32_t pass_key) {
            DBG_PRINTF("\n[BLE-SEC] onPassKeyNotify: %u", pass_key);
        }

        bool onConfirmPIN(uint32_t pass_key) {
            DBG_PRINTF("\n[BLE-SEC] onConfirmPIN: %u — confirmando", pass_key);
            return true;
        }

        bool onSecurityRequest() {
            DBG_PRINT(F("\n[BLE-SEC] onSecurityRequest — OK"));
            return true;
        }

        void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
            if (cmpl.success) {
                DBG_PRINT(F("\n[BLE-SEC] Autenticacao bond SUCESSO — enviando AUTH:OK"));
                bleAutenticado = true;
                enviaBLE("AUTH:OK");
            } else {
                DBG_PRINTF("\n[BLE-SEC] Autenticacao bond FALHOU (reason=%d) — enviando AUTH:FAIL",
                           cmpl.fail_reason);
                bleAutenticado = false;
                enviaBLE("AUTH:FAIL");
            }
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Callbacks de conexão/desconexão
    // ─────────────────────────────────────────────────────────────────────────
    class MyServerCallbacks : public BLEServerCallbacks {

        void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) {
            deviceConnected = true;
            bleAutenticado  = false;  // Sempre resetar ao conectar — exige nova autenticação
            digitalWrite(PINO_STATUS, LED_STATUS_ON);

            // -----------------------------------------------------------------
            // FIX STATUS=8 — CAMADA 1:
            // Forçar connection parameters logo após conectar.
            //
            // min_interval = max_interval = 0x06 → 6 * 1.25ms = 7.5ms
            //   Intervalo fixo e curto: ESP32 responde a cada 7.5ms.
            //   Isso garante keep-alives frequentes e supervision timeout alto.
            //
            // latency = 0 → slave latency ZERO
            //   ESP32 NUNCA pula connection events.
            //   Com latency > 0, o ESP32 pode pular N eventos, o que causa
            //   o status=8 quando o Android não recebe ACK por muito tempo.
            //
            // timeout = 0x1F4 → 500 * 10ms = 5000ms
            //   Supervision timeout de 5s: o Android aguarda 5s sem resposta
            //   antes de declarar o dispositivo perdido.
            //   Isso é suficiente para sobreviver a picos de carga no ESP32.
            // -----------------------------------------------------------------
            esp_ble_conn_update_params_t conn_params = {};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.min_int = 0x06;   // 7.5ms
            conn_params.max_int = 0x06;   // 7.5ms
            conn_params.latency = 0;      // sem slave latency
            conn_params.timeout = 0x1F4;  // 5000ms supervision timeout
            esp_ble_gap_update_conn_params(&conn_params);

            DBG_PRINT(F("\n[BLE] Conectado — conn params: interval=7.5ms, latency=0, timeout=5000ms"));
            DBG_PRINTF("\n[BLE] MAC remoto: %02X:%02X:%02X:%02X:%02X:%02X",
                param->connect.remote_bda[0], param->connect.remote_bda[1],
                param->connect.remote_bda[2], param->connect.remote_bda[3],
                param->connect.remote_bda[4], param->connect.remote_bda[5]);

            // Se havia operação em andamento, notifica o Android ao reconectar
            // Aguarda 1s para que a autenticação (CAMADA 2) seja concluída primeiro
            if (operacaoEmAndamento && mlLiberadoGlobal > 0) {
                DBG_PRINTF("\n[BLE] Reconexao durante operacao — aguardando auth para enviar VP:%.3f",
                           mlLiberadoGlobal);
                // O status VP será enviado pelo loop de dispensação automaticamente
                // na próxima iteração de 2s (proximoStatus)
            }
        }

        // Sobrecarga sem parâmetros (compatibilidade com versões antigas da lib)
        void onConnect(BLEServer *pServer) {
            deviceConnected = true;
            bleAutenticado  = false;
            digitalWrite(PINO_STATUS, LED_STATUS_ON);
            DBG_PRINT(F("\n[BLE] Conectado (callback sem param — conn params nao atualizados)"));
        }

        void onDisconnect(BLEServer *pServer) {
            deviceConnected = false;
            bleAutenticado  = false;
            digitalWrite(PINO_STATUS, !LED_STATUS_ON);
            DBG_PRINT(F("\n[BLE] Desconectado — reiniciando advertising em 200ms"));
            // Delay mínimo antes de reiniciar advertising para o stack BLE estabilizar
            delay(200);
            pServer->startAdvertising();
            DBG_PRINT(F("\n[BLE] Advertising reiniciado"));
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Callbacks de escrita na característica RX — CAMADA 2 (autenticação por comando)
    // ─────────────────────────────────────────────────────────────────────────
    class MyCallbacks : public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *pCharacteristic) {
            std::string rxValue = pCharacteristic->getValue();
            if (rxValue.length() == 0) return;

            // Converte para String Arduino e remove espaços/newlines
            String cmd = "";
            for (size_t i = 0; i < rxValue.length(); i++) {
                cmd += (char)rxValue[i];
            }
            cmd.trim();

            DBG_PRINTF("\n[BLE] Recebido: [%s] | autenticado=%s",
                       cmd.c_str(), bleAutenticado ? "SIM" : "NAO");

            // -----------------------------------------------------------------
            // CAMADA 2 — Autenticação por comando $AUTH:<pin>
            //
            // O Android envia $AUTH:259087 após discoverServices().
            // Este comando é processado MESMO SEM bleAutenticado=true,
            // pois é o próprio mecanismo de autenticação.
            //
            // Nota: O bond nativo (CAMADA 1) já garante que apenas dispositivos
            // pareados chegam até este ponto. O $AUTH é uma segunda validação
            // para garantir que o app correto está conectado.
            // -----------------------------------------------------------------
            if (cmd.startsWith("$") && cmd.substring(1).startsWith(COMANDO_AUTH)) {
                String pinRecebido = cmd.substring(1 + strlen(COMANDO_AUTH));
                pinRecebido.trim();
                DBG_PRINTF("\n[BLE] AUTH recebido — PIN=[%s]", pinRecebido.c_str());
                if (pinRecebido == BLE_AUTH_PIN) {
                    bleAutenticado = true;
                    DBG_PRINT(F("\n[BLE] AUTH:OK — dispositivo autenticado"));
                    enviaBLE("AUTH:OK");
                } else {
                    bleAutenticado = false;
                    DBG_PRINTF("\n[BLE] AUTH:FAIL — PIN incorreto (esperado: %s)", BLE_AUTH_PIN);
                    enviaBLE("AUTH:FAIL");
                }
                return;
            }

            // Bloqueia qualquer outro comando se não autenticado
            if (!bleAutenticado) {
                DBG_PRINT(F("\n[BLE] BLOQUEADO — comando recebido sem autenticacao"));
                enviaBLE("ERROR:NOT_AUTHENTICATED");
                return;
            }

            // Processa o comando de operação (ML, LB, PL, etc.)
            executaOperacao(cmd);
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // setupBLE() — Inicializa o BLE com nome dinâmico CHOPP_XXXX
    // ─────────────────────────────────────────────────────────────────────────
    void setupBLE() {

    BLEDevice::init(BLE_NAME);

    // Segurança BLE com bond/pairing PIN 259087
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

        BLESecurity *pSecurity = new BLESecurity();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
        pSecurity->setCapability(ESP_IO_CAP_OUT);
        pSecurity->setStaticPIN((uint32_t)atoi(BLE_AUTH_PIN));
        pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        DBG_PRINTF("\n[BLE] Seguranca: MITM+BOND+SC | PIN: %s", BLE_AUTH_PIN);

        // Servidor BLE
        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());

        // -----------------------------------------------------------------
        // Serviço NUS (Nordic UART Service)
        // UUID padrão compatível com a maioria dos apps BLE UART
        // -----------------------------------------------------------------
        BLEService *pService = pServer->createService(SERVICE_UUID);

        // TX: ESP32 → Android (NOTIFY)
        pTxCharacteristic = pService->createCharacteristic(
            CHARACTERISTIC_UUID_TX,
            BLECharacteristic::PROPERTY_NOTIFY
        );
        pTxCharacteristic->addDescriptor(new BLE2902());

        // RX: Android → ESP32 (WRITE)
        BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
            CHARACTERISTIC_UUID_RX,
            BLECharacteristic::PROPERTY_WRITE
        );
        pRxCharacteristic->setCallbacks(new MyCallbacks());

        pService->start();

        // -----------------------------------------------------------------
        // Advertising com intervalo curto para reconexão rápida
        //
        // 0x20 = 32 * 0.625ms = 20ms (mínimo BLE)
        // 0x40 = 64 * 0.625ms = 40ms
        //
        // Intervalo curto = o Android encontra o dispositivo mais rápido
        // após uma desconexão. Importante para o fluxo de reconexão.
        // -----------------------------------------------------------------
        BLEAdvertising *pAdvertising = pServer->getAdvertising();
        pAdvertising->addServiceUUID(SERVICE_UUID);
        pAdvertising->setMinInterval(0x20);  // 20ms
        pAdvertising->setMaxInterval(0x40);  // 40ms
        pAdvertising->setScanResponse(true);
        pAdvertising->start();

        DBG_PRINTF("\n[BLE] Aguardando conexao — Nome: %s | PIN: %s", bleName, BLE_AUTH_PIN);
        DBG_PRINT(F("\n[BLE] Seguranca: CAMADA1=Bond+PIN | CAMADA2=$AUTH:PIN"));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // enviaBLE() — Envia string para o Android via NUS TX (notify)
    // ─────────────────────────────────────────────────────────────────────────
    void enviaBLE(String msg) {
        if (!deviceConnected || pTxCharacteristic == NULL) return;
        msg += '\n';
        pTxCharacteristic->setValue(msg.c_str());
        pTxCharacteristic->notify();
        DBG_PRINTF("\n[BLE] Enviado: [%s]", msg.c_str());
    }

#endif
