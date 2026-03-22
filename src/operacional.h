#ifndef OPERACIONAL_H
#define OPERACIONAL_H

#include <Arduino.h>



#endif
#include "config.h"
#ifndef _OPERACIONAL_H_
    #define _OPERACIONAL_H_
    #include <EEPROM.h>
    #include "operaBLE.h"
    #include "esp_task_wdt.h"
    
    void executaOperacao(String cmd);
    void taskLiberaML(void *pvParameters);
    void leConfiguracao();
    void gravaConfiguracao();

    // Variáveis globais de estado da operação em andamento
    // Usadas pelo onConnect() do BLE para enviar status ao reconectar
    extern volatile bool  operacaoEmAndamento;
    extern volatile float mlRestante;
    extern volatile float mlLiberadoGlobal;
#endif    