#include "App_433.h"

App433 My433;

static void delay_us(uint32_t n) { delayMicroseconds(n); }
static void delay_ms(uint32_t n) { vTaskDelay(pdMS_TO_TICKS(n)); }

// ================= 底层 SPI 驱动 =================
void App433::writeByte(uint8_t data) {
    pinMode(PIN_RF_SDIO, OUTPUT); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW); 
        if (data & 0x80) digitalWrite(PIN_RF_SDIO, HIGH);
        else digitalWrite(PIN_RF_SDIO, LOW);
        delay_us(2); digitalWrite(PIN_RF_SCLK, HIGH); delay_us(2); 
        data <<= 1; 
    }
    digitalWrite(PIN_RF_SCLK, LOW); digitalWrite(PIN_RF_SDIO, HIGH); 
}

uint8_t App433::readByte() {
    uint8_t data = 0;
    pinMode(PIN_RF_SDIO, INPUT); 
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_RF_SCLK, LOW); delay_us(2); data <<= 1; 
        digitalWrite(PIN_RF_SCLK, HIGH); 
        if (digitalRead(PIN_RF_SDIO)) data |= 0x01;
        delay_us(2);
    }
    digitalWrite(PIN_RF_SCLK, LOW); return data;
}

void App433::writeReg(uint8_t addr, uint8_t data) {
    digitalWrite(PIN_RF_CSB, LOW); delay_us(2);
    writeByte(addr & 0x7F); writeByte(data);        
    digitalWrite(PIN_RF_CSB, HIGH); 
}

uint8_t App433::readReg(uint8_t addr) {
    uint8_t val;
    digitalWrite(PIN_RF_CSB, LOW); delay_us(2);
    writeByte(addr | 0x80); val = readByte();      
    digitalWrite(PIN_RF_CSB, HIGH); return val;
}

void App433::softReset() { writeReg(0x7F, 0xFF); }

void App433::loadConfig() {
    // 26MHz 晶振 + FSK 433.92 配置 (经验证有效)
    const uint8_t s_data[] = {
        0x00, 0x00, 0x01, 0x66, 0x02, 0xEC, 0x03, 0x1D, 0x04, 0xF0, 0x05, 0x80, 0x06, 0x14, 0x07, 0x08, 
        0x08, 0x91, 0x09, 0x02, 0x0A, 0x02, 0x0B, 0xD0, 0x0C, 0xAE, 0x0D, 0xE0, 0x0E, 0x35, 0x0F, 0x00, 
        0x10, 0x00, 0x11, 0xF4, 0x12, 0x10, 0x13, 0xE2, 0x14, 0x42, 0x15, 0x20, 0x16, 0x00, 0x17, 0x81, 
        0x18, 0x42, 0x19, 0xF3, 0x1A, 0xED, 0x1B, 0x1C, 0x1C, 0x42, 0x1D, 0xDD, 0x1E, 0x3B, 0x1F, 0x1C, 
        0x20, 0xD3, 0x21, 0x64, 0x22, 0x10, 0x23, 0x33, 0x24, 0xD1, 0x25, 0x35, 0x26, 0x0D, 0x27, 0x0A, 
        0x28, 0x9F, 0x29, 0x4B, 0x2A, 0x29, 0x2B, 0x28, 0x2C, 0xC0, 0x2D, 0x28, 0x2E, 0x0A, 0x2F, 0x53, 
        0x30, 0x08, 0x31, 0x00, 0x32, 0xB4, 0x33, 0x00, 0x34, 0x00, 0x35, 0x01, 0x36, 0x00, 0x37, 0x00, 
        0x38, 0x12, 0x39, 0x08, 0x3A, 0x00, 0x3B, 0xAA, 0x3C, 0x16, 0x3D, 0x00, 0x3E, 0x00, 0x3F, 0x00, 
        0x40, 0x00, 0x41, 0x3C, 0x42, 0x7E, 0x43, 0x3C, 0x44, 0x7E, 0x45, 0x05, 0x46, 0x1F, 0x47, 0x00, 
        0x48, 0x00, 0x49, 0x00, 0x4A, 0x00, 0x4B, 0x00, 0x4C, 0x03, 0x4D, 0xFF, 0x4E, 0xFF, 0x4F, 0x60, 
        0x50, 0xFF, 0x51, 0x00, 0x52, 0x09, 0x53, 0x40, 0x54, 0x90, 0x55, 0x70, 0x56, 0xFE, 0x57, 0x06, 
        0x58, 0x00, 0x59, 0x0F, 0x5A, 0x70, 0x5B, 0x00, 0x5C, 0x8A, 0x5D, 0x18, 0x5E, 0x3F, 0x5F, 0x6A
    };
    for(int i=0; i<sizeof(s_data); i+=2) writeReg(s_data[i], s_data[i+1]);
}

void App433::setFreq433() {
    writeReg(0x18, 0x42); writeReg(0x19, 0x71); writeReg(0x1A, 0xCE); writeReg(0x1B, 0x1C);
}

// ================= 业务逻辑 =================

void App433::init() {
    pinMode(PIN_RF_CSB, OUTPUT); pinMode(PIN_RF_FCSB, OUTPUT);
    pinMode(PIN_RF_SCLK, OUTPUT); pinMode(PIN_RF_SDIO, OUTPUT);
    pinMode(PIN_RF_GPIO3, INPUT); 

    digitalWrite(PIN_RF_CSB, HIGH); digitalWrite(PIN_RF_FCSB, HIGH); digitalWrite(PIN_RF_SCLK, LOW);
    
    softReset(); delay_ms(20); loadConfig();
    
    uint8_t tmp = readReg(0x09); writeReg(0x09, (tmp & 0xF8) | 0x02);
    
    writeReg(0x65, 0x10); // GPIO3 DOUT
    setFreq433();
    writeReg(0x60, 0x08); // RX Mode
    delay_ms(10);

    Serial.println("[433] RAW Recorder Ready.");
}

void App433::loop() {
    // 冷却计时
    static unsigned long lastPrint = 0;
    
    // 1. 每 10ms 检查一次信号强度
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10) return;
    lastCheck = millis();

    // 2. 只要信号足够强，立刻开始无脑录制
    int16_t rssi = (int16_t)readReg(0x70) - 128;
    
    if (rssi > -90 && (millis() - lastPrint > 2000)) { // 2秒只能录一次
        Serial.printf("\n>>> 触发! RSSI: %d dBm. 正在录制...\n", rssi);
        
        // 准备数组：存 128 个电平变化的时间
        #define MAX_EDGES 128
        uint32_t durations[MAX_EDGES];
        uint8_t  levels[MAX_EDGES];
        int edgeCount = 0;
        
        // 关中断保护，防止 WiFi 打断录制
        portDISABLE_INTERRUPTS();
        
        unsigned long startTime = micros();
        unsigned long lastEdge = startTime;
        int lastState = digitalRead(PIN_RF_GPIO3);
        
        // 死循环录制 100ms
        while (micros() - startTime < 100000 && edgeCount < MAX_EDGES) {
            int currState = digitalRead(PIN_RF_GPIO3);
            if (currState != lastState) {
                unsigned long now = micros();
                durations[edgeCount] = now - lastEdge;
                levels[edgeCount] = lastState; // 记录刚才那个电平是啥
                edgeCount++;
                
                lastEdge = now;
                lastState = currState;
            }
        }
        
        portENABLE_INTERRUPTS();
        // 录制结束
        
        // 3. 打印分析
        if (edgeCount > 10) {
            Serial.println("Index\tLevel\tTime(us)");
            for (int i = 0; i < edgeCount; i++) {
                // 忽略极短的毛刺 (<30us)
                if (durations[i] > 30) {
                    Serial.printf("%d\t%s\t%lu\n", i, (levels[i] == HIGH ? "HIGH" : "LOW "), durations[i]);
                }
            }
            Serial.println(">>> 录制结束 <<<\n");
            lastPrint = millis(); // 记录时间，冷却
        } else {
            Serial.println(">>> 信号太短或无变化");
        }
    }
}