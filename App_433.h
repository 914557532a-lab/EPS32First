#ifndef APP_433_H
#define APP_433_H

#include <Arduino.h>
#include "Pin_Config.h" 

class App433 {
public:
    void init(); 
    void loop(); 

private:
    // ============ 底层驱动 ============
    void writeByte(uint8_t data); 
    uint8_t readByte(); 
    void writeReg(uint8_t addr, uint8_t val); 
    uint8_t readReg(uint8_t addr); 
    
    // ============ 配置相关 ============
    void softReset(); 
    void loadConfig(); 
    void setFreq433(); 

    // ============ 业务逻辑 ============
    // 解码函数：注意这里有 startIndex 参数
    int decodePulse(uint32_t* durations, int startIndex, int edgeCount, uint8_t* outBuffer);

    // 处理动作：注意这里接收的是 uint8_t 数组，而不是 uint32_t
    void handleAction(uint8_t* data, int bitCount);
};

extern App433 My433;

#endif