#ifndef APP_433_H
#define APP_433_H

#include <Arduino.h>
#include "Pin_Config.h" 

class App433 {
public:
    void init();
    void loop(); 

private:
    void RX_Init();
    void RX_GoReceive();
    
    // 底层 SPI 函数
    void delay_us(uint32_t n);
    void cmt_spi_send(uint8_t data);
    uint8_t cmt_spi_recv();
    void writeReg(uint8_t addr, uint8_t data);
    uint8_t readReg(uint8_t addr);
    void readFifo(uint8_t* buf, uint16_t len);
};

extern App433 My433;

#endif