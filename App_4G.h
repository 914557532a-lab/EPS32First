#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 使用 SIM7600 驱动定义，兼容 LE270 系列的常用 AT 指令
#define TINY_GSM_MODEM_SIM7600 
#include <TinyGsmClient.h>

/**
 * @brief 接收状态机枚举
 * 用于在字节流中精准定位 +MIPREAD 的数据主体
 */
enum RxState {
    ST_SEARCH,        // 寻找起始符号 '+'
    ST_MATCH_HEADER,  // 匹配关键字 (如 +MIPREAD: 或 +MIPPUSH:)
    ST_SKIP_ID,       // 跳过连接 ID
    ST_READ_LEN,      // 解析后续数据长度
    ST_READ_DATA      // 读取原始二进制数据
};

class App4G {
public:
    void init(); 
    void powerOn();
    bool connectTCP(const char* host, uint16_t port);
    void closeTCP();
    
    // 发送数据接口
    bool sendData(const uint8_t* data, size_t len);
    bool sendData(uint8_t* data, size_t len);
    
    // 核心读取接口：支持主动轮询与超时处理
    int  readData(uint8_t* buf, size_t maxLen, uint32_t timeout_ms);
    
    // 状态机处理逻辑
    void process4GStream();
    int  popCache();
    
    bool isConnected();
    TinyGsmClient& getClient(); 
    HardwareSerial* getClientSerial() { return _serial4G; }

    // 基础连接检测
    bool connect(unsigned long timeout_ms = 15000L) {
        unsigned long start = millis();
        while (millis() - start < timeout_ms) {
            if (_modem && _modem->isNetworkConnected()) return true;
            vTaskDelay(pdMS_TO_TICKS(1000)); 
        }
        return false;
    }

    String getIMEI() { return _modem ? _modem->getIMEI() : ""; }
    void sendRawAT(String cmd) { if (_serial4G) _serial4G->println(cmd); }
    int getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 99; }

private:
    HardwareSerial* _serial4G = &Serial2; 
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;
    
    // 解析状态机变量
    RxState g_st = ST_SEARCH;
    bool _has_pending_data = false;
    
    // 内部辅助函数
    bool waitResponse(const char* expected, uint32_t timeout);
    bool checkBaudrate(uint32_t baud);
};

extern App4G My4G;

#endif