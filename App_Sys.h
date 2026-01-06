#ifndef APP_SYS_H
#define APP_SYS_H

#include <Arduino.h>
#include "Pin_Config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 定义按键动作类型
enum KeyAction {
    KEY_NONE,
    KEY_SHORT_PRESS,      // 短按松开
    KEY_LONG_PRESS_START, // 长按触发
    KEY_LONG_PRESS_HOLD,  // 长按保持
    KEY_LONG_PRESS_END    // 长按结束
};
// --- [新增] 网络任务消息结构 ---
enum NetEventType {
    NET_EVENT_NONE,
    NET_EVENT_UPLOAD_AUDIO  // 上传录音指令
};

struct NetMessage {
    NetEventType type;
    uint8_t* data;      // 指向音频数据的指针
    size_t len;         // 数据长度
};

// 声明全局队列句柄，方便其他文件 extern 引用
extern QueueHandle_t NetQueue_Handle;

class AppSys {
public:
    void init();
    
    // --- 传感器 ---
    float getTemperatureC(); 
    uint32_t getFreeHeap();

    // --- 核心逻辑 ---
    // 专门给 RTOS 任务调用的轮询函数 (修复了这里缺失实现的问题)
    void scanLoop(); 

    // 获取按键动作 (状态机)
    KeyAction getKeyAction();

private:
    // 内部变量
    uint32_t _pressStartTime = 0;
    bool _isLongPressHandled = false;
};

extern AppSys MySys;

#endif