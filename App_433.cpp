#include "App_433.h"

App433 My433;

// --- 辅助函数 ---
// 微秒级延时 (用于软件SPI时序)
void delay_us(uint32_t n) {
    delayMicroseconds(n);
}

// 毫秒级延时 (RTOS 专用，让出 CPU)
void delay_ms_rtos(uint32_t n) {
    vTaskDelay(pdMS_TO_TICKS(n));
}

// --- 底层 SPI/SDIO 驱动 (软件模拟 3-Wire SPI) ---

void App433::writeByte(uint8_t data) {
    pinMode(PIN_RF_SDIO, OUTPUT); //SDIO是半双工模式，在写入的时候需要切换成输出模式
    
    for (int i = 0; i < 8; i++) {//轮询8次
        digitalWrite(PIN_RF_SCLK, LOW); //准备写入数据
        
        if (data & 0x80) {//0x80=1000 0000 与位操作，如果最高位是1，则与上1000 0000 还是1，则执行digitalWrite(PIN_RF_SDIO, HIGH); 发送一个高电平，如果是0这与完后是0执行digitalWrite(PIN_RF_SDIO, LOW);
            digitalWrite(PIN_RF_SDIO, HIGH);
        } else {
            digitalWrite(PIN_RF_SDIO, LOW);
        }
        
        delay_us(1); // 保持电平稳定
        digitalWrite(PIN_RF_SCLK, HIGH); // 上升沿写入
        delay_us(1); //保持电平稳定
        
        data <<= 1; //向左移位1位，低位补0
    }
    
    digitalWrite(PIN_RF_SCLK, LOW);   //回到默认
    digitalWrite(PIN_RF_SDIO, HIGH); // 释放
}

uint8_t App433::readByte() {
    uint8_t data = 0;//用来存储读取到的数据
    
    pinMode(PIN_RF_SDIO, INPUT); // 半双工所以需要切换为输入模式
    
    for (int i = 0; i < 8; i++) {//一次读取8位
        digitalWrite(PIN_RF_SCLK, LOW); //SCLK拉低准备读取
        delay_us(1); //电平稳定
        
        data <<= 1; //移位
        
        digitalWrite(PIN_RF_SCLK, HIGH); // 上升沿读取
        
        if (digitalRead(PIN_RF_SDIO)) {//如果发送过来的是一个高电平那么执行或位操作 0000 0001，在最低为添加1，否则不加直接移位
            data |= 0x01;
        }
        
        delay_us(1);//电平稳定
    }
    
    digitalWrite(PIN_RF_SCLK, LOW); //回到默认
    return data;
}

void App433::writeReg(uint8_t addr, uint8_t data) {
    digitalWrite(PIN_RF_CSB, LOW); // 选中寄存器
    
    writeByte(addr & 0x7F); // 写地址 (最高位0)
    writeByte(data);        // 写数据
    
    digitalWrite(PIN_RF_CSB, HIGH); 
}

uint8_t App433::readReg(uint8_t addr) {
    uint8_t data;
    
    digitalWrite(PIN_RF_CSB, LOW); 
    
    writeByte(addr | 0x80); // 读地址 (最高位1)
    data = readByte();      // 读数据
    
    digitalWrite(PIN_RF_CSB, HIGH); 
    return data;
}

void App433::writeFifo(uint8_t* data, uint8_t len) {//发送信号
    digitalWrite(PIN_RF_FCSB, LOW); // 选中 FIFO
    
    for(int i = 0; i < len; i++) {
        writeByte(data[i]); 
    }
    
    digitalWrite(PIN_RF_FCSB, HIGH); 
}

void App433::softReset() {//在0x7F中写入0xFF就可以重启
    writeReg(0x7F, 0xFF); 
}

// --- 核心配置 ---

void App433::loadConfig() {
    // CMT2300A 433.92MHz FSK 配置表 (通用默认值)
    // 频率: 433.92MHz | 调制: FSK | 速率: 10kbps | 偏差: 20kHz
    // 这是从 RFPDK 导出的标准 Bank 配置
    const uint8_t rfpdk_regs[] = {
        // [CMT Bank]
        0x00, 0x00, 0x01, 0x66, 0x02, 0xEC, 0x03, 0x1C, 0x04, 0xF0, 0x05, 0x80, 0x06, 0x14, 0x07, 0x08,
        0x08, 0x91, 0x09, 0x02, 0x0A, 0x02, 0x0B, 0xD0,
        // [System Bank]
        0x0C, 0xAE, 0x0D, 0xE0, 0x0E, 0x35, 0x0F, 0x00, 0x10, 0x00, 0x11, 0xF4, 0x12, 0x10, 0x13, 0xE2,
        0x14, 0x42, 0x15, 0x20, 0x16, 0x00, 0x17, 0x81,
        // [Frequency Bank - 433.92MHz]
        0x18, 0x42, 0x19, 0x71, 0x1A, 0xCE, 0x1B, 0x1C, 0x1C, 0x42, 0x1D, 0x5B, 0x1E, 0x1C, 0x1F, 0x1C,
        // [Data Rate Bank - 10kbps]
        0x20, 0x32, 0x21, 0x18, 0x22, 0x00, 0x23, 0x99, 0x24, 0xC1, 0x25, 0x9B, 0x26, 0x06, 0x27, 0x0A,
        0x28, 0x9F, 0x29, 0x39, 0x2A, 0x29, 0x2B, 0x29, 0x2C, 0xC0, 0x2D, 0x51, 0x2E, 0x2A, 0x2F, 0x53,
        0x30, 0x00, 0x31, 0x00, 0x32, 0xB4, 0x33, 0x00, 0x34, 0x00, 0x35, 0x01, 0x36, 0x00, 0x37, 0x00,
        // [Baseband Bank]
        0x38, 0x12, 0x39, 0x08, 0x3A, 0x00, 0x3B, 0xAA, 0x3C, 0x02, 0x3D, 0x00, 0x3E, 0x00, 0x3F, 0x00,
        0x40, 0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0xD4, 0x44, 0x2D, 0x45, 0x00, 0x46, 0x1F, 0x47, 0x00,
        0x48, 0x00, 0x49, 0x00, 0x4A, 0x00, 0x4B, 0x00, 0x4C, 0x00, 0x4D, 0x00, 0x4E, 0x00, 0x4F, 0x60,
        0x50, 0xFF, 0x51, 0x00, 0x52, 0x00, 0x53, 0x1F, 0x54, 0x10,
        // [TX Bank]
        0x55, 0x50, 0x56, 0x26, 0x57, 0x03, 0x58, 0x00, 0x59, 0x42, 0x5A, 0xB0, 0x5B, 0x00, 0x5C, 0x37,
        0x5D, 0x0A, 0x5E, 0x3F, 0x5F, 0x7F
    };

    // 循环写入所有配置
    for (int i = 0; i < sizeof(rfpdk_regs); i += 2) {
        writeReg(rfpdk_regs[i], rfpdk_regs[i+1]);
    }
}

// --- 上层应用接口 ---

void App433::init() {
    // 1. 设置引脚模式
    pinMode(PIN_RF_CSB, OUTPUT); 
    pinMode(PIN_RF_FCSB, OUTPUT);
    pinMode(PIN_RF_SCLK, OUTPUT);
    pinMode(PIN_RF_SDIO, OUTPUT);
    pinMode(PIN_RF_GPIO3, INPUT); // 状态/中断引脚

    // 2. 默认电平
    digitalWrite(PIN_RF_CSB, HIGH);
    digitalWrite(PIN_RF_FCSB, HIGH);
    digitalWrite(PIN_RF_SCLK, LOW);
    
    Serial.println("[433] Initializing CMT2300A...");
    
    // 3. 软复位
    softReset();
    delay_ms_rtos(20); // 使用 RTOS 延时等待复位
    
    // 4. 检查芯片 ID (0x0C 寄存器通常是非零值)
    uint8_t check = readReg(0x0C); 
    Serial.printf("[433] Chip Check Reg(0x0C): 0x%02X\n", check);

    if(check == 0x00 || check == 0xFF) {
        Serial.println("[433] Error: Chip communication failed!");
    } else {
        Serial.println("[433] Chip detected.");
    }

    // 5. 写入寄存器配置
    loadConfig();
    
    // 6. 再次校准或进入休眠
    // 默认进入 SLEEP 模式等待发送指令
    writeReg(0x66, 0x80); 
    Serial.println("[433] Init Done.");
}

void App433::sendPacket(uint8_t* data, uint8_t len) {
    Serial.println("[433] Sending packet...");
    
    // 1. 进入 STBY 模式
    writeReg(0x66, 0x20); 
    delay_us(500); // 稍微等稳一点
    
    // 2. 写入 FIFO
    writeFifo(data, len); 
    
    // 3. 触发 TX 模式
    // 这里的 0x40 或 0x60 取决于配置中的 GPIO 触发方式，通常 0x60 是直接 SPI 触发 TX
    writeReg(0x66, 0x60); 
    
    // 4. 等待发送完成
    // 在 RTOS 下，这里使用 vTaskDelay 代替死等
    // 发送时间 = (数据长度 * 8 / 波特率) + 前导码时间
    // 10kbps 下发 10 字节大约需要 10-20ms
    delay_ms_rtos(50); 
    
    // 5. 回到 SLEEP 模式省电
    writeReg(0x66, 0x80); 
}

void App433::loop() {
    // 检查 GPIO3 状态 (在配置中通常映射为 PKT_DONE 或 SYNC_OK)
    if (digitalRead(PIN_RF_GPIO3) == HIGH) {
        // 这里只是简单的信号检测演示
        // 实际接收需要：切入 RX 模式 -> 读 FIFO -> 清中断
        
        static uint32_t lastPrint = 0;
        if (millis() - lastPrint > 1000) {
            Serial.println("[433] Signal Detected on GPIO3!");
            lastPrint = millis();
        }
    }
}