#include "App_433.h"
#include <RCSwitch.h>

App433 My433;
RCSwitch mySwitch = RCSwitch();

void delay_us(uint32_t n) { delayMicroseconds(n); }
void delay_ms_rtos(uint32_t n) { vTaskDelay(pdMS_TO_TICKS(n)); }

// --- SPI 驱动 ---
void App433::writeByte(uint8_t data) {
    pinMode(PIN_RF_SDIO, OUTPUT); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW); 
        if (data & 0x80) digitalWrite(PIN_RF_SDIO, HIGH);
        else digitalWrite(PIN_RF_SDIO, LOW);
        delay_us(1); 
        digitalWrite(PIN_RF_SCLK, HIGH); 
        delay_us(1); 
        data <<= 1; 
    }
    digitalWrite(PIN_RF_SCLK, LOW);   
    digitalWrite(PIN_RF_SDIO, HIGH); 
}

uint8_t App433::readByte() {
    uint8_t data = 0;
    pinMode(PIN_RF_SDIO, INPUT); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW); 
        delay_us(1); 
        data <<= 1; 
        digitalWrite(PIN_RF_SCLK, HIGH); 
        if (digitalRead(PIN_RF_SDIO)) data |= 0x01;
        delay_us(1);
    }
    digitalWrite(PIN_RF_SCLK, LOW); 
    return data;
}

void App433::writeReg(uint8_t addr, uint8_t data) {
    digitalWrite(PIN_RF_CSB, LOW); 
    writeByte(addr & 0x7F); 
    writeByte(data);        
    digitalWrite(PIN_RF_CSB, HIGH); 
}

uint8_t App433::readReg(uint8_t addr) {
    uint8_t data;
    digitalWrite(PIN_RF_CSB, LOW); 
    writeByte(addr | 0x80); 
    data = readByte();      
    digitalWrite(PIN_RF_CSB, HIGH); 
    return data;
}

void App433::softReset() {
    writeReg(0x7F, 0xFF); 
}

// --- 核心配置 (26MHz OOK) ---
void App433::loadConfig() {
    const uint8_t rfpdk_regs[] = {
        // [CMT Bank]
        0x00, 0x00, 0x01, 0x66, 0x02, 0xEC, 0x03, 0x1D, 0x04, 0xF0, 0x05, 0x80, 0x06, 0x14, 0x07, 0x08,
        0x08, 0x91, 
        // 0x09 (CUS_CMT10) 和 0x0A (CUS_CMT11) 控制晶振
        0x09, 0x02, 
        // 【关键修改】0x0A 原值 0x02，改为 0x06 增强驱动能力试试 (不同批次芯片可能不同)
        // 0x0A, 0x02, 
        0x0A, 0x06, // 尝试增大驱动
        0x0B, 0xD0,
        
        // [System Bank]
        0x0C, 0xAE, 0x0D, 0xE0, 0x0E, 0x35, 0x0F, 0x00, 0x10, 0x00, 0x11, 0xF4, 0x12, 0x10, 0x13, 0xE2,
        0x14, 0x42, 0x15, 0x20, 0x16, 0x00, 0x17, 0x81,
        
        // [Frequency Bank] - 26MHz -> 433.92MHz
        0x18, 0x42, 0x19, 0x71, 0x1A, 0xCE, 0x1B, 0x1C, 0x1C, 0x42, 0x1D, 0x5B, 0x1E, 0x1C, 0x1F, 0x1C,
        
        // [Data Rate Bank]
        0x20, 0x54, 0x21, 0x28, 0x22, 0xB0, 0x23, 0xCC, 0x24, 0x00, 0x25, 0x00, 0x26, 0x00, 0x27, 0x00,
        0x28, 0x00, 0x29, 0x00, 0x2A, 0x00, 0x2B, 0x29, 0x2C, 0xC0, 0x2D, 0x64, 0x2E, 0x19, 0x2F, 0x5B,
        0x30, 0x07, 0x31, 0x00, 0x32, 0x50, 0x33, 0x2D, 0x34, 0x00, 0x35, 0x01, 0x36, 0x05, 0x37, 0x05,
        
        // [Baseband Bank]
        0x38, 0x10, 0x39, 0x06, 0x3A, 0x00, 0x3B, 0xAA, 0x3C, 0x26, 0x3D, 0x00, 0x3E, 0x00, 0x3F, 0x00,
        0x40, 0x00, 0x41, 0x3C, 0x42, 0x7E, 0x43, 0x3C, 0x44, 0x7E, 0x45, 0x05, 0x46, 0x1F, 0x47, 0x00,
        0x48, 0x00, 0x49, 0x00, 0x4A, 0x00, 0x4B, 0x00, 0x4C, 0x03, 0x4D, 0xFF, 0x4E, 0xFF, 0x4F, 0x60,
        0x50, 0xFF, 0x51, 0x00, 0x52, 0x04, 0x53, 0x09, 
        0x54, 0x29, // 全端口输出
        
        // [TX Bank]
        0x55, 0x55, 0x56, 0x9A, 0x57, 0x0C, 0x58, 0x00, 0x59, 0x09, 0x5A, 0xB0, 0x5B, 0x00, 0x5C, 0x8A,
        0x5D, 0x18, 0x5E, 0x3F, 0x5F, 0x6A
    };

    for (int i = 0; i < sizeof(rfpdk_regs); i += 2) {
        writeReg(rfpdk_regs[i], rfpdk_regs[i+1]);
    }
}

// --- 初始化 ---
void App433::init() {
    pinMode(PIN_RF_CSB, OUTPUT); 
    pinMode(PIN_RF_FCSB, OUTPUT);
    pinMode(PIN_RF_SCLK, OUTPUT);
    pinMode(PIN_RF_SDIO, OUTPUT);
    pinMode(PIN_RF_GPIO3, INPUT_PULLUP);

    digitalWrite(PIN_RF_CSB, HIGH);
    digitalWrite(PIN_RF_FCSB, HIGH);
    digitalWrite(PIN_RF_SCLK, LOW);
    
    Serial.println("[433] Init 26MHz (High Drive)...");
    softReset();
    delay_ms_rtos(20); 
    
    uint8_t check = readReg(0x0C); 
    if(check == 0x00 || check == 0xFF) {
        Serial.println("[433] Error: Chip not found!");
        return;
    }
    Serial.printf("[433] Chip ID: 0x%02X\n", check);

    loadConfig();
    
    // 强制开启 LFOSC
    uint8_t reg07 = readReg(0x07);
    writeReg(0x07, reg07 | 0x20);
    
    // 强制 GPIO 输出 DOUT
    writeReg(0x54, 0x29); 
    
    // --- 关键步骤：分步锁定测试 ---
    // 1. 先进 STBY
    writeReg(0x66, 0x20);
    delay_ms_rtos(10);
    
    // 2. 尝试进 RFS (RF Synthesizer, 状态 0x03)
    // 这一步只启动晶振和PLL，不开启接收机
    // 如果这一步能锁定(状态=0x03)，说明晶振和频率对了
    Serial.println("[433] Trying to Lock PLL (Go to RFS)...");
    writeReg(0x66, 0x60); 
    delay_ms_rtos(20);
    
    uint8_t status = readReg(0x68);
    if(status == 0x03) {
        Serial.println("[433] PLL Locked! (Status=RFS)");
    } else {
        Serial.printf("[433] PLL Failed to Lock! Status=0x%02X\n", status);
    }

    // 3. 最后尝试进 RX
    Serial.println("[433] Going to RX...");
    writeReg(0x66, 0x40);
    delay_ms_rtos(10);    

    mySwitch.enableReceive(PIN_RF_GPIO3);
    Serial.println("[433] Init Done.");
}

void App433::loop() {
    if (mySwitch.available()) {
        Serial.println("========================================");
        Serial.printf(">>> [SUCCESS] 收到信号: %lu / %d bit\n", 
            mySwitch.getReceivedValue(), mySwitch.getReceivedBitlength());
        Serial.println("========================================");
        mySwitch.resetAvailable();
    }
    
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 2000) {
        lastCheck = millis();
        int16_t rssi = (int16_t)readReg(0x0B) - 128;
        uint8_t status = readReg(0x68);
        int pinVal = digitalRead(PIN_RF_GPIO3);
        
        String statusStr;
        if(status == 0x00) statusStr = "IDLE (失败)";
        else if(status == 0x02) statusStr = "STBY";
        else if(status == 0x03) statusStr = "RFS (PLL锁定)";
        else if(status == 0x06) statusStr = "RX (正常)";
        else statusStr = String(status, HEX);

        Serial.printf("[诊断] 状态: %s | RSSI: %d | GPIO3: %d\n", statusStr.c_str(), rssi, pinVal);
        
        // 自动重启机制
        if (status == 0x00) {
            Serial.println(">>> 芯片挂了，尝试重启 RX...");
            writeReg(0x66, 0x20); // STBY
            delay_ms_rtos(2);
            writeReg(0x66, 0x40); // RX
        }
    }
}