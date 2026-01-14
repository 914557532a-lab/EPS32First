#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TINY_GSM_MODEM_SIM7600 
#define TINY_GSM_DEBUG Serial 

#include <TinyGsmClient.h>

class App4G {
public:
    void init(); 
    void powerOn();
    bool connect(unsigned long timeout_ms = 30000L); 
    bool isConnected();
    String getIMEI();
    TinyGsmClient& getClient(); 
    void sendRawAT(String cmd);
    int getSignalCSQ();

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;

    String _apn = "cmiot"; 
    String _user = "";
    String _pass = "";

    // [新增] 验证标志：确保 connect() 至少完整运行过一次
    bool _is_verified = false;

    bool checkIP_manual();  
    bool ensureNetOpen();   
    void checkDNS();        
};

extern App4G My4G;

#endif