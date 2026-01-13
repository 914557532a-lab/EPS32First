#ifndef APP_SYS_H
#define APP_SYS_H

#include <Arduino.h>
#include "Pin_Config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h> // 确保包含队列定义

// --- 1. 定义按键动作 ---
enum KeyAction {
    KEY_NONE,
    KEY_SHORT_PRESS,      // 短按松开
    KEY_LONG_PRESS_START, // 长按触发
    KEY_LONG_PRESS_HOLD,  // 长按保持
    KEY_LONG_PRESS_END    // 长按结束
};

// --- 2. 定义网络消息结构 ---
enum NetEventType {
    NET_EVENT_NONE,
    NET_EVENT_UPLOAD_AUDIO  // 上传录音指令
};

struct NetMessage {
    NetEventType type;
    uint8_t* data;      // 指向音频数据的指针
    size_t len;         // 数据长度
};

// 声明全局队列句柄
extern QueueHandle_t NetQueue_Handle;

// --- 3. AppSys 类定义 ---
class AppSys {
public:
    void init();
    
    // 传感器
    float getTemperatureC(); 
    uint32_t getFreeHeap();

    // 核心循环
    void scanLoop(); 

    // 按键获取
    KeyAction getKeyAction();

private:
    uint32_t _pressStartTime = 0;
    bool _isLongPressHandled = false;
};

extern AppSys MySys;

#endif