#include "operaPagina.h"

#ifdef USAR_PAGINA_CONFIG

extern config_t configuracao;

String stringHexCor(uint32_t c) {
    String cod = String(c,HEX);
    while ( cod.length() < 6 ) {
        cod = "0" + cod;
    }
    cod = "#" + cod;
    return cod;
}

// Server
AsyncWebServer server(80);

void setupPagina() {
    // Define a página de configuração
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.on("/post", HTTP_POST, [](AsyncWebServerRequest *request){
        String message;
        
        // wifi ssid
        if (request->hasParam("wifi_ssid", true)) {
            message = request->getParam("wifi_ssid", true)->value();
            message.toCharArray(configuracao.wifiSSID, message.length()+1);
        }

        // wifi psw
        if (request->hasParam("wifi_psw", true)) {
            message = request->getParam("wifi_psw", true)->value();
            message.toCharArray(configuracao.wifiPass, message.length()+1);
        }
        request->send(200, "text/plain", "Salvo com sucesso!!!");
        //gravaConfiguracao();
        delay(1000);
        ESP.restart();
    });
    
    server.on("/wifi_psw", HTTP_GET, [](AsyncWebServerRequest * request) {
        request->send(200, "text/plain", String(configuracao.wifiPass));
    });

    server.on("/wifi_ssid", HTTP_GET, [](AsyncWebServerRequest * request) {
        request->send(200, "text/plain", String(configuracao.wifiSSID));
    });
    
    // Inicia o servidor web
    server.begin();
    
}
#endif