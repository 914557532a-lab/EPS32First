#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TINY_GSM_MODEM_SIM7600 
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

    bool connectTCP(const char* host, uint16_t port);
    bool sendData(const uint8_t* data, size_t len);
    int  readData(uint8_t* buf, size_t maxLen, uint32_t timeout_ms);
    void closeTCP();

    // 【修正】直接在头文件定义，防止 undefined reference 错误
    HardwareSerial* getClientSerial() { return _serial4G; }

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;
    String _apn = "cmiot";
    bool _is_verified = false;

    bool waitResponse(String expected, int timeout);
    bool checkBaudrate(uint32_t baud);
};

extern App4G My4G;

#endif