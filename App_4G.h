/**
 * @file App_4G.h
 * @brief 4G控制头文件 (已修复 checkBaudrate 声明)
 */
#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// [注意] 虽然这里保留了定义，但我们后续代码会绕过它手动发指令
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

    // --- [新增] 手动 Socket 控制函数 (Fibocom 专用) ---
    bool connectTCP(const char* host, uint16_t port);
    bool sendData(const uint8_t* data, size_t len);
    int  readData(uint8_t* buf, size_t maxLen, uint32_t timeout_ms);
    void closeTCP();

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;

    String _apn = "cmiot"; // 移动物联网卡
    
    bool _is_verified = false;

    // 辅助函数
    bool waitResponse(String expected, int timeout);
    bool checkIP_manual(); 
    
    // 【关键修复】这里补上了 checkBaudrate 的声明
    bool checkBaudrate(uint32_t baud);
};

extern App4G My4G;

#endif