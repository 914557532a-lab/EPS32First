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
    
    // TCP 相关
    bool connectTCP(const char* host, uint16_t port);
    void closeTCP();

    // 发送函数重载
    bool sendData(const uint8_t* data, size_t len);
    bool sendData(uint8_t* data, size_t len);

    // [核心] 智能读取 (非阻塞/缓冲/支持超时)
    int  readData(uint8_t* buf, size_t maxLen, uint32_t timeout_ms);

    // 状态机处理
    void process4GStream();
    int popCache();
    void resetParser();

    // 辅助功能
    bool isConnected();
    TinyGsmClient& getClient(); 
    HardwareSerial* getClientSerial() { return _serial4G; }

    // =============================================================
    // 内联实现兼容性接口 (解决 .ino 报错)
    // =============================================================
    bool connect(unsigned long timeout_ms = 15000L) {
        unsigned long start = millis();
        Serial.print("[4G] Checking Network... ");
        while (millis() - start < timeout_ms) {
            if (_modem && _modem->isNetworkConnected()) {
                Serial.println("OK! (Connected)");
                return true;
            }
            Serial.print(".");
            vTaskDelay(pdMS_TO_TICKS(1000)); 
        }
        Serial.println(" Timeout!");
        return false;
    }

    String getIMEI() { return _modem ? _modem->getIMEI() : ""; }
    void sendRawAT(String cmd) { if (_serial4G) _serial4G->println(cmd); }
    int getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 99; }

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;

    RxState g_st = ST_SEARCH;
    
    // [新增] 贪婪读取标志：标记缓冲区虽空但可能仍有数据在网络侧
    bool _has_pending_data = false;

    // 内部函数
    bool waitResponse(const char* expected, uint32_t timeout);
    bool checkBaudrate(uint32_t baud);
};

extern App4G My4G;

#endif