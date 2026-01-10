#ifndef APP_433_H
#define APP_433_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <RCSwitch.h> // 【重要】请务必安装 RCSwitch 库

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class App433 {
public:
    void init(); 
    void loop(); 

private:
    void writeByte(uint8_t data); 
    uint8_t readByte(); 
    
    void writeReg(uint8_t addr, uint8_t data); 
    uint8_t readReg(uint8_t addr); 
    
    void softReset(); 
    void loadConfig(); 
    
    void setRxMode();
};

extern App433 My433;

#endif