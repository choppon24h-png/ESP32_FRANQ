#include "config.h"
#if !defined(_OPERA_RFID_H_) && defined(USAR_RFID)
    #define _OPERA_RFID_H_
    #include <MFRC522.h> 
    
    #include "operaBLE.h"
    
    void taskRFID(void *pvParameters);
    bool tagPresenteRFID();
    String codigoTagRFID();
#endif    