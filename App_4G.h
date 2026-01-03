#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class App4G {
public:
    void init(); // 只做引脚配置，绝不执行耗时操作
    
    // 4G 模块开机时序 (耗时操作，必须在 Task 中跑)
    void powerOn();
    
    void hardwareReset();

    // 发送 AT 指令并等待回应 (阻塞式，但会出让 CPU)
    String sendAT(String command, uint32_t timeout_ms = 1000);

    void loopPassthrough();

    bool isNetConnected();

    String getIMEI();

private:
    HardwareSerial* _serial = &Serial2; 
    // 私有变量存储 IMEI
    String _cachedIMEI = "";
    // 内部解析函数
    void fetchIMEI();
};

extern App4G My4G;

#endif