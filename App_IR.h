#ifndef APP_IR_H
#define APP_IR_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 定义一个红外事件结构体，用于发送给主控逻辑
struct IREvent {
    uint32_t protocol; // 协议类型 (NEC, SONY, etc.)
    uint64_t value;    // 解码后的数值 (例如 0xFFA25D)
    uint16_t bits;     // 位数
};

class AppIR {
public:
    void init();
    
    // 负责接收信号，并处理重复码逻辑
    void loop();
    void sendNEC(uint32_t data);//发射红外命令
    void sendTestSignal();

private:
    IRrecv* _irRecv = nullptr;
    IRsend* _irSend = nullptr;
    decode_results _results;
};

extern AppIR MyIR;

#endif