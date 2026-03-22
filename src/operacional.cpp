
// Includes padrão
#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <cstring>

#include "config.h"
#include "operacional.h"
#include "ble_protocol.h"
#include "protocol.h"

void executaOperacao(String comando)
{
    Serial.println("Executando operação:");
    Serial.println(comando);

    // Aqui entra sua lógica de operação
}

/* 
    *** Apenas para observar ***
    Sensor de fluxo YF-S401
    Precision (Flow rate - pulse output) 0.3 ~ 6L / min ± 3%
    1L = 5880 pulsos
    6L = 35280 pulsos
    
    6L/min = 35280 / 60 => 588 pulsos/seg
    um cliclo = 1000ms / 588 => 1.700680272ms/pulso
    1L demora 5880 * 1.700680272 = 9999.99999936ms => 9.999999999s ~= 10s

    *** CORREÇÃO BLE — VÁLVULA PERSISTENTE ***
    
    PROBLEMA IDENTIFICADO NO LOG (pasted_content_2.txt):
    
    1. TIMER_OUT_SENSOR = 2000ms era muito curto.
       O timeout do sensor fechava a válvula em 2s sem pulsos,
       mesmo durante uma reconexão BLE legítima.
    
    2. A taskLiberaML fechava a válvula ao atingir o timeout,
       sem verificar se o BLE estava em processo de reconexão.
    
    SOLUÇÃO IMPLEMENTADA:
    
    A. TIMER_OUT_SENSOR aumentado para 30000ms (30s) no config.h.
       Isso dá tempo suficiente para o Android reconectar via BLE
       (backoff máximo de 15s + tempo de bond + discoverServices).
    
    B. A taskLiberaML agora verifica deviceConnected antes de fechar
       a válvula por timeout. Se o BLE está desconectado, aguarda
       até que reconecte (até BLE_RECONEXAO_TIMEOUT_MS = 60s).
    
    C. Adicionada variável global mlRestante para que o Android
       possa consultar quantos ML ainda faltam após reconexão.
       O ESP32 envia "VP:<mlRestante>" ao reconectar.
    
    D. Ao reconectar (onConnect), o ESP32 verifica se há uma
       operação em andamento e envia o status atual automaticamente.
*/

extern config_t configuracao;
extern QueueHandle_t listaLiberarML;
extern TaskHandle_t taskRFIDHandle;

// Declaração externa do estado BLE (definido em protocol.h)
extern OperationState g_opState;

volatile uint32_t contadorPulso  = 0;
volatile uint32_t quantidadePulso = 0;
volatile int64_t  horaPulso      = 0;

// ─── Estado global da operação em andamento ───────────────────────────────────
// Permite que o onConnect() do BLE saiba se há dispensação em curso
volatile bool  operacaoEmAndamento = false;
volatile float mlRestante          = 0.0;
volatile float mlLiberadoGlobal    = 0.0;

// Timeout máximo aguardando reconexão BLE durante dispensação (60s)
#define BLE_RECONEXAO_TIMEOUT_MS 60000ULL

void IRAM_ATTR fluxoISR() {
    contadorPulso++;
    horaPulso = esp_timer_get_time();
    if ((quantidadePulso) && (!(contadorPulso < quantidadePulso))) {
        digitalWrite(PINO_RELE, !RELE_ON);
        detachInterrupt(digitalPinToInterrupt(PINO_SENSOR_FLUSO));
    }
}

void taskLiberaML(void *pvParameters) {
    String statusRetorno;
    float mlLiberado = 0.0;
    float pulsoML    = 0.0;
    float tempoDecorridoS = 0;
    uint32_t ml = 0;
    int64_t tempoInicio = 0;
    unsigned long proximoStatus = 0;
    DBG_PRINT(F("\n[OPER] Task taskLiberaML iniciada"));
    
    for (;;) {
        vTaskDelay(50);
        if (xQueueReceive(listaLiberarML, &ml, 0) == pdTRUE) {
            if (ml) {
                pulsoML = (float)configuracao.pulsosLitro / 1000.0;
                DBG_PRINT(F("\n[OPER] liberando (Pulsos/ML): "));
                DBG_PRINT(pulsoML);
                
                tempoDecorridoS = 0.0;
                mlLiberado      = 0.0;
                
                if (ml == 0xFFFFFFFF) {
                    ml = 0;
                    quantidadePulso = 0;
                } else {
                    quantidadePulso = (uint32_t)(pulsoML * (float)ml);
                    DBG_PRINT(F("\n[OPER] Liberando (ML): "));
                    DBG_PRINT(ml);
                    DBG_PRINT(F("\n[OPER] liberando (Pulsos): "));
                    DBG_PRINT(quantidadePulso);
                }
                
                contadorPulso = 0;
                attachInterrupt(digitalPinToInterrupt(PINO_SENSOR_FLUSO), fluxoISR, RISING);
                
                // Aciona válvula
                digitalWrite(PINO_RELE, RELE_ON);
                tempoInicio = esp_timer_get_time();
                horaPulso   = tempoInicio;
                
                // Timeout do sensor: configuracao.timeOut (padrão agora 30000ms)
                int64_t timeOutSensor = tempoInicio + ((int64_t)configuracao.timeOut * 1000LL);
                
                // Marca operação em andamento para que onConnect() saiba
                operacaoEmAndamento = true;
                mlLiberadoGlobal    = 0.0;
                mlRestante          = (ml > 0) ? (float)ml : 0.0;
                
                // ─── Loop principal de dispensação ────────────────────────────
                while ((contadorPulso < quantidadePulso) || (quantidadePulso == 0)) {
                    vTaskDelay(50);
                    
                    // ─── Verifica timeout do sensor ───────────────────────────
                    // FIX: Só fecha a válvula por timeout se o BLE estiver
                    // conectado. Se desconectado, aguarda reconexão (até 60s).
                    if (esp_timer_get_time() >= timeOutSensor) {
                        if (!g_opState.bleConectado) {
                            // BLE desconectado durante dispensação — aguarda reconexão
                            DBG_PRINT(F("\n[OPER] Timeout sensor, mas BLE desconectado — aguardando reconexao..."));
                            
                            int64_t inicioEspera = esp_timer_get_time();
                            bool reconectou = false;
                            
                            while ((esp_timer_get_time() - inicioEspera) < (BLE_RECONEXAO_TIMEOUT_MS * 1000LL)) {
                                vTaskDelay(200);
                                if (g_opState.bleConectado) {
                                    reconectou = true;
                                    DBG_PRINT(F("\n[OPER] BLE reconectado! Continuando dispensacao..."));
                                    // Reinicia timeout do sensor após reconexão
                                    timeOutSensor = esp_timer_get_time() + ((int64_t)configuracao.timeOut * 1000LL);
                                    horaPulso = esp_timer_get_time();
                                    break;
                                }
                            }
                            
                            if (!reconectou) {
                                // Timeout máximo de reconexão atingido — fecha válvula
                                DBG_PRINT(F("\n[OPER] Timeout maximo de reconexao BLE atingido — fechando valvula"));
                                break;
                            }
                        } else {
                            // BLE conectado mas sem fluxo — timeout real do sensor
                            DBG_PRINT(F("\n[OPER] Timeout sensor com BLE conectado — fechando valvula"));
                            break;
                        }
                    }
                    
                    // ─── Envia status parcial a cada 2s ──────────────────────
                    if (millis() > proximoStatus) {
                        proximoStatus = millis() + 2000UL;
                        tempoDecorridoS = (float)(horaPulso - tempoInicio) / 1000000.0;
                        if (contadorPulso) {
                            mlLiberado = (float)contadorPulso / pulsoML;
                        }
                        // Atualiza globais para onConnect() poder consultar
                        mlLiberadoGlobal = mlLiberado;
                        if (ml > 0) {
                            mlRestante = (float)ml - mlLiberado;
                            if (mlRestante < 0) mlRestante = 0;
                        }
                        
                        #ifdef USAR_ESP32_UART_BLE
                            if (g_opState.bleConectado) {
                                statusRetorno = COMANDO_VP + String(mlLiberado, 3);
                                bleProtocol_send(statusRetorno.c_str());
                            }
                        #endif
                    }
                }
                // ─── Fim do loop de dispensação ───────────────────────────────
                
                // Fecha válvula e desanexa interrupção
                digitalWrite(PINO_RELE, !RELE_ON);
                detachInterrupt(digitalPinToInterrupt(PINO_SENSOR_FLUSO));
                
                // Marca operação como concluída
                operacaoEmAndamento = false;
                mlRestante          = 0.0;
                mlLiberadoGlobal    = 0.0;
                
                // Calcula ML final
                if (contadorPulso && pulsoML > 0) {
                    mlLiberado = (float)contadorPulso / pulsoML;
                }
                
                // Envia status final ao Android
                #ifdef USAR_ESP32_UART_BLE
                    if (g_opState.bleConectado) {
                        statusRetorno = COMANDO_QP + String(contadorPulso);
                        bleProtocol_send(statusRetorno.c_str());
                        
                        statusRetorno = COMANDO_ML;
                        if (quantidadePulso > 0 && contadorPulso >= quantidadePulso) {
                            // Dispensação completa
                            statusRetorno += String(ml);
                        } else {
                            // Dispensação parcial (timeout ou interrompida)
                            statusRetorno += String(mlLiberado, 3);
                        }
                        bleProtocol_send(statusRetorno.c_str());
                        
                        DBG_PRINT(F("\n[OPER] Status final enviado: "));
                        DBG_PRINT(statusRetorno);
                    } else {
                        DBG_PRINT(F("\n[OPER] BLE desconectado ao fim — status final nao enviado"));
                    }
                #endif
                
                DBG_PRINT(F("\n[OPER] Liberado (mL): "));
                DBG_PRINT(mlLiberado, 3);
                DBG_PRINT(F("\n[OPER] Tempo (S): "));
                DBG_PRINT(tempoDecorridoS);
                DBG_PRINT(F("\n[OPER] Quantidade pulsos: "));
                DBG_PRINT(contadorPulso);
            }
        }
    }
}

// Recupera configuração gravada na EEPROM
void leConfiguracao() {  
    String stemp;
    DBG_PRINT(F("[OPER] Lendo configuração"));
    EEPROM.begin(sizeof(config_t));
    EEPROM.get(0, configuracao);  
  
    // Inicializa com configurações padrão quando as configurações não foram
    // gravadas pela primeira vez ou em caso de reset
    if (configuracao.magicFlag != MAGIC_FLAG_EEPROM) {    
        DBG_PRINT(F(", carregando configuração de fábrica"));
        memset(&configuracao, 0, sizeof(config_t));
        configuracao.magicFlag = MAGIC_FLAG_EEPROM;
        configuracao.modoAP    = 0;
        
        // WiFi 
        stemp = WIFI_DEFAULT_SSID;
        stemp.toCharArray(configuracao.wifiSSID, stemp.length() + 1);
        stemp = WIFI_DEFAULT_PSW;
        stemp.toCharArray(configuracao.wifiPass, stemp.length() + 1);

        configuracao.pulsosLitro = (uint32_t)PULSO_LITRO;
        // FIX: timeout padrão aumentado para 30s para suportar reconexão BLE
        configuracao.timeOut = (uint32_t)TIMER_OUT_SENSOR;
    }
    DBG_PRINTLN();
}

// Salva configuração na EEPROM
void gravaConfiguracao() {
    DBG_PRINT(F("\n[OPER] Gravando configuração "));
    EEPROM.put(0, configuracao);
    if (EEPROM.commit()) {
        DBG_PRINT(F("OK"));
    } else {
        DBG_PRINT(F(" *** Falha"));
    }
}
