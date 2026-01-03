#ifndef APP_433_H
#define APP_433_H

#include <Arduino.h>
#include "Pin_Config.h" 

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class App433 {
public:
    void init(); 
    void sendPacket(uint8_t* data, uint8_t len); 
    void loop(); 

private:
    void writeByte(uint8_t data); 
    uint8_t readByte(); 
    
    void writeReg(uint8_t addr, uint8_t data); 
    uint8_t readReg(uint8_t addr); 
    
    void writeFifo(uint8_t* data, uint8_t len); 
    
    void softReset(); 
    void loadConfig(); 
};

extern App433 My433;

#endif