#ifndef APP_433_H
#define APP_433_H

#include <Arduino.h>
#include "Pin_Config.h" 

class App433 {
public:
    void init(); 
    void loop(); 

private:
    // 底层驱动
    void writeByte(uint8_t data); 
    uint8_t readByte(); 
    void writeReg(uint8_t addr, uint8_t val); 
    uint8_t readReg(uint8_t addr); 
    
    // 配置相关
    void softReset(); 
    void loadConfig(); 
    void setFreq433(); 
};

extern App433 My433;

#endif