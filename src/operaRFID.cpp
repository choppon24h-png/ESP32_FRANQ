#include "operaRFID.h"
#include "ble_protocol.h"
#ifdef USAR_RFID

extern config_t configuracao;
extern QueueHandle_t listaLiberarML;

MFRC522 mfrc522(PINO_RC522_SSEL, PINO_RC522_RSET);

void taskRFID(void *pvParameters) {
    DBG_PRINT( F( "\n[RFID] Task iniciada"));
    vTaskDelay(2000);
    DBG_PRINT( F( "\n[RFID] Iniciando Sensor"));
    SPI.begin(PINO_RC522_SCLK,PINO_RC522_MISO,PINO_RC522_MOSI);    
    mfrc522.PCD_Init(PINO_RC522_SSEL, PINO_RC522_RSET);
    vTaskDelay(50);
    
    byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
    if (( v == 0xFF )||( v == 0x00 )) {
        mfrc522.PCD_Init(PINO_RC522_SSEL, PINO_RC522_RSET);
        vTaskDelay(50);
    }    
    #ifdef debug_debug
        DBG_PRINT( "\n[RFID] " );
        mfrc522.PCD_DumpVersionToSerial(); 
    #endif

    for (;;){
        vTaskDelay(500);
        if (tagPresenteRFID()){
            String resp = codigoTagRFID();
            if (String(configuracao.rfidMaster) == resp.c_str()) {
                resp += ":MASTER";
                uint32_t quantidade = 0xFFFFFFFF;
                if (xQueueSend(listaLiberarML,&quantidade,0) == pdTRUE ) {
                    
                }
            }
            #ifdef USAR_ESP32_UART_BLE
                resp = COMANDO_ID + resp;
                bleProtocol_send(resp.c_str());           
            #endif 
        }
    }
}


bool tagPresenteRFID() {
    static unsigned long proximaLeitura = 0;
    if ( millis() < proximaLeitura ) {
        return false;
    }

    if ( !mfrc522.PICC_IsNewCardPresent()) {
        return false;
    }

    if ( !mfrc522.PICC_ReadCardSerial()) {
        return false;
    }

    // Mostra dados da TAG pela serial 
    #ifdef debug_debug
        //mfrc522.PICC_DumpToSerial(&(mfrc522.uid));
    #endif

    proximaLeitura = millis() + 2000UL;

    return true;
}

String codigoTagRFID() {
    String rfidCode = "";
  
    // Pega id RFID
    byte rfidAddr[4] = {0};
    for (byte i = 0; i < 4; i++)   {
        rfidAddr[i] = mfrc522.uid.uidByte[i];
        if (rfidAddr[i] < 0x10) {
            rfidCode += "0";
        }
        rfidCode += String(rfidAddr[i],HEX);
    }
    DBG_PRINT( "\n[RFID] Tag: ");
    DBG_PRINT(rfidCode);  
    return rfidCode;
}
#endif