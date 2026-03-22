#include "config.h"
#if !defined(_OPERA_PAGINA_H_) && defined(USAR_PAGINA_CONFIG)
    #define _OPERA_PAGINA_H_

    #include <ESPAsyncWebServer.h>

    void setupPagina();

    const char index_html[] PROGMEM = R"rawliteral(<!DOCTYPE html>
    <!DOCTYPE html>
<html lang="pt-BR">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Configuração</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            background-color: #f4f4f4;
            margin: 0;
            padding: 20px;
        }
        h1 {
            color: #333;
            text-align: center;
        }
        form {
            max-width: 600px;
            margin: 0 auto;
            background-color: #fff;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);
        }
        fieldset {
            border: 1px solid #ddd;
            border-radius: 8px;
            padding: 15px;
            margin-bottom: 20px;
        }
        legend {
            font-weight: bold;
            color: #333;
            padding: 0 10px;
        }
        label {
            display: block;
            margin: 10px 0 5px;
            color: #555;
        }
        input[type="text"],
        input[type="tel"],
        input[type="number"],
        input[type="password"],
        select {
            width: 100%;
            padding: 10px;
            margin: 5px 0 15px;
            border: 1px solid #ddd;
            border-radius: 4px;
            font-size: 16px;
        }
        input[type="submit"],
        input[type="reset"] {
            display: inline-block;
            padding: 10px 20px;
            margin: 10px 5px;
            border: none;
            border-radius: 4px;
            background-color: #4CAF50;
            color: white;
            font-size: 16px;
            cursor: pointer;
        }
        input[type="submit"]:hover,
        input[type="reset"]:hover {
            background-color: #45a049;
        }
        .button-container {
            text-align: center;
        }
    </style>
    </head>
    <script>
      function getField(cmp) {
        var xhttp=new XMLHttpRequest();
        xhttp.open("GET","/"+cmp,false);
        xhttp.send();
        return xhttp.responseText;
      }
      function getData() {        
        document.getElementById("wifi_ssid").value=getField("wifi_ssid");
        document.getElementById("wifi_psw").value=getField("wifi_psw");        
      }
    function togglePassword() {
        const input = document.getElementById('wifi_psw');
        input.type = input.type === 'password' ? 'text' : 'password';
    }
    </script>
      <body onload="getData();">
        <h1>Settings</h1>
        <form action="/post" method="POST">
          <fieldset>
            <legend>WiFi</legend>
            <label>SSID</label>
            <input type="text" id="wifi_ssid" name="wifi_ssid" required value=""><br>
            <label>Password</label>
            <input type="password" id="wifi_psw" name="wifi_psw" required value="">
            <label><input type="checkbox" onclick="togglePassword()"> Mostrar senha</label>
          </fieldset>
          <div class="button-container">
                <input type="reset" value="Reset">
                <input type="submit" value="Submit">
            </div>          
        </form>        
      </body>
      </html>)rawliteral";
#endif