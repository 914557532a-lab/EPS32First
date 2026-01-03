#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AppWiFi {
public:
    void init();
    
    // 连接到指定网络 (非阻塞，瞬间返回)
    void connect(const char* ssid, const char* password);

    bool isConnected();
    wl_status_t getStatus();
    String getIP();
    String getMac();
    int getRSSI();
    
    // 专门用于调试打印状态，不包含时间控制
    void logStatus(); 

private:
    // 移除了 _lastCheckTime 和 _checkInterval
    // 这些由 FreeRTOS 任务控制
};

extern AppWiFi MyWiFi;

#endif