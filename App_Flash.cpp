#include "App_Flash.h"

AppFlash MyFlash;

#define FLASH_SPI_SPEED 10000000 

// 辅助延时函数
static void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void AppFlash::init() {
    // 1. Flash CS 引脚
    pinMode(PIN_FLASH_CS, OUTPUT); 
    digitalWrite(PIN_FLASH_CS, HIGH); 

    // 2. SPI 初始化
    // 注意：TFT 库通常也会初始化 SPI。如果两者共用同一个 SPI 实体 (Arduino 的 SPI 全局对象)，
    // 这里其实不需要重复 begin，但为了保险起见，保留无妨。
    SPI.begin(PIN_FLASH_CLK, PIN_FLASH_MISO, PIN_FLASH_MOSI, -1);
    
    Serial.println("[Flash] Initialized.");
}

// --- 核心冲突保护 ---

void AppFlash::_spi_start() {
    // 【第1步】拿锁！ (死等，直到屏幕把总线让出来)
    // 如果不加这句，屏幕正在刷新的瞬间读取 Flash，必挂。
    xSemaphoreTake(xGuiSemaphore, portMAX_DELAY);

    // 【第2步】切换 GPIO 模式
    // 因为 TFT 库刚刚可能把 GPIO 3 用作了 DC 输出
    // 现在我们要把它强行切回 MISO 输入
    pinMode(PIN_FLASH_MISO, INPUT); 

    // 【第3步】开启 SPI 事务
    SPI.beginTransaction(SPISettings(FLASH_SPI_SPEED, MSBFIRST, SPI_MODE0));
    
    // 【第4步】选中 Flash
    digitalWrite(PIN_FLASH_CS, LOW); 
}

void AppFlash::_spi_end() {
    // 【第1步】释放 Flash
    digitalWrite(PIN_FLASH_CS, HIGH);
    
    // 【第2步】结束事务
    SPI.endTransaction();

    // 【第3步】还锁！
    // 让屏幕任务可以继续画图
    xSemaphoreGive(xGuiSemaphore);
}

// --- 业务功能 (大部分保持原样，因为逻辑都被封装在 start/end 里了) ---

uint32_t AppFlash::readJedecID() {
    uint32_t id = 0;
    _spi_start(); // <--- 这里面会自动拿锁
    SPI.transfer(CMD_READ_ID);
    uint8_t id1 = SPI.transfer(0x00);
    uint8_t id2 = SPI.transfer(0x00);
    uint8_t id3 = SPI.transfer(0x00);
    _spi_end();   // <--- 这里面会自动还锁
    id = (id1 << 16) | (id2 << 8) | id3;
    return id;
}

void AppFlash::readData(uint32_t addr, uint8_t *buf, uint32_t len) {
    _spi_start();
    SPI.transfer(CMD_READ_DATA);
    SPI.transfer((addr >> 16) & 0xFF);
    SPI.transfer((addr >> 8) & 0xFF);
    SPI.transfer(addr & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = SPI.transfer(0x00);
    }
    _spi_end();
}

void AppFlash::waitBusy() {
    uint8_t status;
    
    // 这里需要频繁读状态，我们采用“拿锁 -> 读一次 -> 立即还锁 -> 睡一会”的策略
    // 这样做是为了避免 Flash 在擦除时长时间霸占总线，导致屏幕卡死几百毫秒
    
    do {
        // 1. 快速读取状态
        xSemaphoreTake(xGuiSemaphore, portMAX_DELAY); // 拿锁
        
        pinMode(PIN_FLASH_MISO, INPUT); // 确保 MISO 模式
        SPI.beginTransaction(SPISettings(FLASH_SPI_SPEED, MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_FLASH_CS, LOW);
        SPI.transfer(CMD_READ_STATUS1);
        status = SPI.transfer(0x00);
        digitalWrite(PIN_FLASH_CS, HIGH);
        SPI.endTransaction();
        
        xSemaphoreGive(xGuiSemaphore); // 还锁！让屏幕有机会刷新
        
        // 2. 如果忙，让出 CPU 给其他任务 (UI/WiFi)
        if (status & 0x01) {
            vTaskDelay(pdMS_TO_TICKS(2)); // 睡 2ms
        }
        
    } while (status & 0x01);
}

void AppFlash::eraseSector(uint32_t addr) {
    Serial.printf("[Flash] Erasing Sector 0x%X...\n", addr);
    
    // 1. 写使能
    _spi_start();
    SPI.transfer(CMD_WRITE_ENABLE);
    _spi_end();

    // 2. 发送指令
    _spi_start();
    SPI.transfer(CMD_SECTOR_ERASE);
    SPI.transfer((addr >> 16) & 0xFF);
    SPI.transfer((addr >> 8) & 0xFF);
    SPI.transfer(addr & 0xFF);
    _spi_end();

    // 3. 等待 (这是耗时最久的一步)
    // 这里的 waitBusy 已经优化过，不会卡死 UI
    waitBusy();
    Serial.println("[Flash] Erase Done.");
}

void AppFlash::writeData(uint32_t addr, uint8_t *buf, uint32_t len) {
    _spi_start();
    SPI.transfer(CMD_WRITE_ENABLE);
    _spi_end();

    _spi_start();
    SPI.transfer(CMD_PAGE_PROGRAM);
    SPI.transfer((addr >> 16) & 0xFF);
    SPI.transfer((addr >> 8) & 0xFF);
    SPI.transfer(addr & 0xFF);
    for(uint32_t i=0; i<len; i++) {
        SPI.transfer(buf[i]);
    }
    _spi_end();
    
    waitBusy();
    Serial.println("[Flash] Write Done.");
}