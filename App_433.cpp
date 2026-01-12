#include "App_433.h"
#include "App_Display.h" 

App433 My433;

// ================= 寄存器地址 =================
#define CMT2300A_CUS_MODE_CTL    0x60
#define CMT2300A_CUS_FIFO_CTL    0x69
#define CMT2300A_CUS_INT_CLR1    0x6A
#define CMT2300A_CUS_INT_CLR2    0x6B
#define CMT2300A_CUS_FIFO_CLR    0x6C
#define CMT2300A_CUS_IO_SEL      0x65
#define CMT2300A_CUS_INT_EN      0x68
#define CMT2300A_CUS_INT2_CTL    0x67
#define CMT2300A_CUS_PKT17       0x48 

const uint8_t CMT2300A_S_Data[] = {
    0x00, 0x00, 0x01, 0x66, 0x02, 0xEC, 0x03, 0x1D, 0x04, 0xF0, 0x05, 0x80, 
    0x06, 0x14, 0x07, 0x08, 0x08, 0x91, 0x09, 0x02, 0x0A, 0x02, 0x0B, 0xD0,
    0x0C, 0xAE, 0x0D, 0xE0, 0x0E, 0x35, 0x0F, 0x00, 0x10, 0x00, 0x11, 0xF4, 
    0x12, 0x10, 0x13, 0xE2, 0x14, 0x42, 0x15, 0x20, 0x16, 0x00, 0x17, 0x81,
    0x18, 0x42, 0x19, 0xF3, 0x1A, 0xED, 0x1B, 0x1C, 0x1C, 0x42, 0x1D, 0xDD, 
    0x1E, 0x3B, 0x1F, 0x1C,
    0x20, 0xD3, 0x21, 0x64, 0x22, 0x10, 0x23, 0x33, 0x24, 0xD1, 0x25, 0x35, 
    0x26, 0x0D, 0x27, 0x0A, 0x28, 0x9F, 0x29, 0x4B, 0x2A, 0x29, 0x2B, 0x28, 
    0x2C, 0xC0, 0x2D, 0x28, 0x2E, 0x0A, 0x2F, 0x53, 0x30, 0x08, 0x31, 0x00, 
    0x32, 0xB4, 0x33, 0x00, 0x34, 0x00, 0x35, 0x01, 0x36, 0x00, 0x37, 0x00, 
    0x38, 0x12, 0x39, 0x08, 0x3A, 0x00, 0x3B, 0xAA, 0x3C, 0x16, 0x3D, 0x00, 
    0x3E, 0x00, 0x3F, 0x00, 0x40, 0x00, 0x41, 0x3C, 0x42, 0x7E, 0x43, 0x3C, 
    0x44, 0x7E, 0x45, 0x05, 0x46, 0x1F, 0x47, 0x00, 0x48, 0x00, 0x49, 0x00, 
    0x4A, 0x00, 0x4B, 0x00, 0x4C, 0x03, 0x4D, 0xFF, 0x4E, 0xFF, 0x4F, 0x60, 
    0x50, 0xFF, 0x51, 0x00, 0x52, 0x09, 0x53, 0x40, 0x54, 0x90,
    0x55, 0x70, 0x56, 0xFE, 0x57, 0x06, 0x58, 0x00, 0x59, 0x0F, 0x5A, 0x70, 
    0x5B, 0x00, 0x5C, 0x8A, 0x5D, 0x18, 0x5E, 0x3F, 0x5F, 0x6A
};

// ================= 底层实现 =================
void App433::delay_us(uint32_t n) { delayMicroseconds(n); }

void App433::cmt_spi_send(uint8_t data) {
    pinMode(PIN_RF_SDIO, OUTPUT);
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW);
        if (data & 0x80) digitalWrite(PIN_RF_SDIO, HIGH);
        else digitalWrite(PIN_RF_SDIO, LOW);
        delay_us(2); 
        digitalWrite(PIN_RF_SCLK, HIGH); 
        delay_us(2);
        data <<= 1;
    }
    digitalWrite(PIN_RF_SCLK, LOW); 
    digitalWrite(PIN_RF_SDIO, HIGH);
}

uint8_t App433::cmt_spi_recv() {
    uint8_t data = 0;
    pinMode(PIN_RF_SDIO, INPUT_PULLUP); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW); 
        delay_us(2); 
        data <<= 1;
        digitalWrite(PIN_RF_SCLK, HIGH); 
        if (digitalRead(PIN_RF_SDIO)) data |= 0x01;
        delay_us(2);
    }
    digitalWrite(PIN_RF_SCLK, LOW); 
    return data;
}

void App433::writeReg(uint8_t addr, uint8_t data) {
    digitalWrite(PIN_RF_CSB, LOW); 
    delay_us(4); 
    cmt_spi_send(addr & 0x7F); 
    cmt_spi_send(data);
    delay_us(2);
    digitalWrite(PIN_RF_CSB, HIGH);
}

uint8_t App433::readReg(uint8_t addr) {
    uint8_t val;
    digitalWrite(PIN_RF_CSB, LOW); 
    delay_us(4);
    cmt_spi_send(addr | 0x80); 
    val = cmt_spi_recv();
    delay_us(2);
    digitalWrite(PIN_RF_CSB, HIGH); 
    return val;
}

void App433::readFifo(uint8_t* buf, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        digitalWrite(PIN_RF_FCSB, LOW);
        delay_us(2); 
        buf[i] = cmt_spi_recv();
        delay_us(2);
        digitalWrite(PIN_RF_FCSB, HIGH);
        delay_us(4);
    }
}

// ================= 高层逻辑 =================

void App433::RX_Init() {
    pinMode(PIN_RF_CSB, OUTPUT); pinMode(PIN_RF_FCSB, OUTPUT);
    pinMode(PIN_RF_SCLK, OUTPUT); pinMode(PIN_RF_SDIO, OUTPUT);
    pinMode(PIN_RF_GPIO3, INPUT);

    digitalWrite(PIN_RF_CSB, HIGH); digitalWrite(PIN_RF_FCSB, HIGH); digitalWrite(PIN_RF_SCLK, LOW);
    
    writeReg(0x7F, 0xFF); // Soft Reset
    delay(20);
    
    for(int i=0; i<sizeof(CMT2300A_S_Data); i+=2) {
        writeReg(CMT2300A_S_Data[i], CMT2300A_S_Data[i+1]);
    }
    
    writeReg(CMT2300A_CUS_IO_SEL, 0x20);   // GPIO3 = INT2
    writeReg(CMT2300A_CUS_INT2_CTL, 0x07); // INT2 = PKT_OK
    writeReg(CMT2300A_CUS_INT_EN, 0x01);   // Enable PKT_DONE
    
    Serial.println("[433] Init Done.");
}

void App433::RX_GoReceive() {
    writeReg(CMT2300A_CUS_MODE_CTL, 0x02); // STBY
    writeReg(CMT2300A_CUS_FIFO_CLR, 0x02); // Clear RX FIFO
    writeReg(CMT2300A_CUS_INT_CLR1, 0xFF);
    writeReg(CMT2300A_CUS_INT_CLR2, 0xFF);
    writeReg(CMT2300A_CUS_FIFO_CTL, 0x00); // FIFO Read Mode
    writeReg(CMT2300A_CUS_MODE_CTL, 0x08); // Go RX
}

void App433::init() {
    RX_Init();
    RX_GoReceive();
}

void App433::loop() {
    if (digitalRead(PIN_RF_GPIO3) == HIGH) {
        
        writeReg(CMT2300A_CUS_MODE_CTL, 0x02); // 1. 先 STBY
        
        uint8_t buffer[32];
        memset(buffer, 0, 32);
        readFifo(buffer, 20); // 2. 读取数据
        
        // 转换为 String
        String rxData = "";
        for(int i=0; i<20; i++) {
            if(buffer[i] >= 32 && buffer[i] <= 126) rxData += (char)buffer[i];
        }
        
        Serial.println("[433] RX: " + rxData);

        // ================= 防抖逻辑 Start =================
        static uint32_t lastActionTime = 0;
        uint32_t now = millis();
        const uint32_t COOLDOWN = 1000; // 冷却时间 1000ms (1秒)

        if (rxData.indexOf("TOGGLE") >= 0) {
            // 只有当距离上次操作超过 1秒 时才执行
            if (now - lastActionTime > COOLDOWN) {
                MyDisplay.toggleBacklight();
                lastActionTime = now;
                Serial.println(">>> Action: TOGGLE Executed");
            } else {
                Serial.println(">>> Ignored: Cooling down...");
            }
        }
        // ================= 防抖逻辑 End =================

        // 3. 继续接收
        RX_GoReceive();
    }
}