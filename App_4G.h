#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TINY_GSM_MODEM_SIM7600 
#include <TinyGsmClient.h>

// 状态机状态定义
enum RxState {
    ST_SEARCH,
    ST_SKIP_ID,
    ST_READ_LEN,
    ST_READ_DATA
};

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

    // TCP 相关
    bool connectTCP(const char* host, uint16_t port);
    void closeTCP();

    // 发送函数重载
    bool sendData(const uint8_t* data, size_t len);
    bool sendData(uint8_t* data, size_t len);

    int  readData(uint8_t* buf, size_t maxLen, uint32_t timeout_ms);

    // 流处理函数
    void process4GStream();
    int popCache();

    HardwareSerial* getClientSerial() { return _serial4G; }

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;
    String _apn = "cmiot";
    bool _is_verified = false;

    // 状态机变量 (改为纯变量，无 String)
    RxState g_st = ST_SEARCH;
    
    // 内部函数
    bool waitResponse(const char* expected, uint32_t timeout);
    bool checkBaudrate(uint32_t baud);
};

extern App4G My4G;

#endif