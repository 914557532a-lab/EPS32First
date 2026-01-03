#ifndef APP_FLASH_H
#define APP_FLASH_H

#include <Arduino.h>
#include <SPI.h>
#include "Pin_Config.h" 
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h> // 引入信号量/互斥锁

// 引用在 App_Display.cpp (或者 main.cpp) 中定义的全局锁
// 注意：为了逻辑清晰，你可以把这个锁改名为 xSpiMutex，
// 但为了兼容之前的代码，我们先暂时复用 xGuiSemaphore
extern SemaphoreHandle_t xGuiSemaphore;

// Flash 指令 (保持不变)
#define CMD_WRITE_ENABLE  0x06
#define CMD_WRITE_DISABLE 0x04
#define CMD_READ_DATA     0x03
#define CMD_PAGE_PROGRAM  0x02
#define CMD_SECTOR_ERASE  0x20 
#define CMD_BLOCK_ERASE   0xD8 
#define CMD_READ_ID       0x9F 
#define CMD_READ_STATUS1  0x05

class AppFlash {
public:
    void init();
    uint32_t readJedecID();
    void readData(uint32_t addr, uint8_t *buf, uint32_t len);
    void writeData(uint32_t addr, uint8_t *buf, uint32_t len);
    void eraseSector(uint32_t addr);

private:
    void waitBusy(); 
    
    // 我们将把锁的逻辑封装在这两个函数里
    void _spi_start();
    void _spi_end();
};

extern AppFlash MyFlash;

#endif