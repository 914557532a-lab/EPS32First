#ifndef APP_4G_H
#define APP_4G_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =========================================================
//  配置 TinyGSM 适配的模组型号
//  Fibocom LE270-EU / L610 系列通常兼容 SIM7600 指令集
// =========================================================
#define TINY_GSM_MODEM_SIM7600 

#include <TinyGsmClient.h>

class App4G {
public:
    // 初始化引脚和串口，不进行耗时操作
    void init(); 
    
    // 4G 模块开机时序 (包含上电、拉PWRKEY、等待握手)，耗时操作
    void powerOn();
    
    // 拨号连接移动网络 (设置 APN 并激活 PDP 上下文)
    bool connect(); 
    
    // 检查网络是否连接 (GPRS/LTE 是否就绪)
    bool isConnected();

    // 获取模块 IMEI
    String getIMEI();

    // 获取 Client 实例，用于传递给 App_Server 进行网络通信
    TinyGsmClient& getClient(); 

private:
    // 硬件串口指针 (指向 Serial2)
    HardwareSerial* _serial4G = &Serial2; 
    
    // TinyGSM 核心对象指针 (建议动态分配，避免静态初始化顺序问题)
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _client = nullptr;

    // 移动物联网卡 APN 配置
    // 移动通常是 "cmnet" 或 "cmiot"，电信 "ctnet"，联通 "3gnet"
    String _apn = "cmnet"; 
    String _user = "";
    String _pass = "";
};

extern App4G My4G;

#endif