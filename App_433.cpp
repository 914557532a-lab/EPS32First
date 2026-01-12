#include "App_433.h"

App433 My433;

static void delay_us(uint32_t n) { delayMicroseconds(n); }
static void delay_ms(uint32_t n) { vTaskDelay(pdMS_TO_TICKS(n)); }

// ================= 底层 SPI 驱动 (保持不变) =================
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
    // 26MHz 晶振 + FSK 433.92 配置
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

void App433::init() {
    pinMode(PIN_RF_CSB, OUTPUT); pinMode(PIN_RF_FCSB, OUTPUT);
    pinMode(PIN_RF_SCLK, OUTPUT); pinMode(PIN_RF_SDIO, OUTPUT);
    pinMode(PIN_RF_GPIO3, INPUT); 

    digitalWrite(PIN_RF_CSB, HIGH); digitalWrite(PIN_RF_FCSB, HIGH); digitalWrite(PIN_RF_SCLK, LOW);
    
    Serial.println("[433] Resetting Chip...");
    softReset(); delay_ms(20); 
    loadConfig();
    
    uint8_t tmp = readReg(0x09); writeReg(0x09, (tmp & 0xF8) | 0x02);
    
    writeReg(0x65, 0x10); // GPIO3 DOUT
    setFreq433();
    writeReg(0x60, 0x08); // RX Mode
    delay_ms(10);

    Serial.println("[433] Working Decoder Ready.");
}

// ================= 核心解码器 =================
int App433::decodePulse(uint32_t* durations, int startIndex, int edgeCount, uint8_t* outBuffer) {
    int bitIndex = 0;
    int byteIndex = 0;
    memset(outBuffer, 0, 8); 

    // 从 startIndex + 2 开始，只看 High (偶数跳变)
    for (int i = startIndex + 2; i < edgeCount; i += 2) {
        if (durations[i] > 2000) break; // 遇到下一个Sync，结束
        
        uint32_t t = durations[i]; 
        
        // 宽松判决: 
        // 1: > 800us
        // 0: 30us ~ 800us (包含你遥控器的极短信号)
        
        if (t > 800 && t < 1800) {
            outBuffer[byteIndex] |= (0x80 >> bitIndex);
            bitIndex++;
        } 
        else if (t > 20 && t <= 800) { // 门槛降到20us，防止漏抓
            bitIndex++;
        }
        else {
            // 异常脉宽，忽略或结束
            // break; 
        }

        if (bitIndex >= 8) {
            bitIndex = 0;
            byteIndex++;
            if (byteIndex >= 8) break; 
        }
    }
    return (byteIndex * 8 + bitIndex);
}

void App433::handleAction(uint8_t* data, int bitCount) {
    if (bitCount < 16) return; // 噪音太短不打印

    Serial.printf("[RX] %d bits: ", bitCount);
    for(int i=0; i < (bitCount+7)/8; i++) Serial.printf("%02X ", data[i]);
    
    // 匹配 Mode 4
    if (bitCount >= 20) {
        uint8_t cmd_h = data[2];
        uint8_t cmd_l = (bitCount >= 24) ? data[3] : 0;

        if (cmd_h == 0x08 && cmd_l == 0x78) Serial.print(" -> 【上调 / 开启泵油】");
        else if (cmd_h == 0x02 && cmd_l == 0x44) Serial.print(" -> 【下调 / 停止泵油】");
        else if (cmd_h == 0x05 && cmd_l == 0x04) Serial.print(" -> 【关机】");
        else if (cmd_h == 0x11 && cmd_l == 0x58) Serial.print(" -> 【开机】");
        
        // 匹配 Mode 1
        else if (cmd_h == 0x08) Serial.print(" -> 【上调 (Mode1)】");
        else if (cmd_h == 0x02) Serial.print(" -> 【下调 (Mode1)】");
        else if (cmd_h == 0x01) Serial.print(" -> 【关机 (Mode1)】");
        else if (cmd_h == 0x04) Serial.print(" -> 【开机 (Mode1)】");
    }
    Serial.println();
}

void App433::loop() {
    static unsigned long lastPrint = 0;
    static unsigned long lastCheck = 0;

    if (millis() - lastCheck < 2) return;
    lastCheck = millis();

    int16_t rssi = (int16_t)readReg(0x70) - 128;
    
    // 触发录制
    if (rssi > -90 && (millis() - lastPrint > 200)) { 
        
        #define MAX_EDGES 2500
        static uint32_t durations[MAX_EDGES]; 
        int edgeCount = 0;
        
        unsigned long startTime = micros();
        unsigned long lastEdgeTime = startTime;
        int lastState = digitalRead(PIN_RF_GPIO3);
        
        while ((micros() - startTime < 150000) && (edgeCount < MAX_EDGES)) {
            int currState = digitalRead(PIN_RF_GPIO3);
            if (currState != lastState) {
                unsigned long now = micros();
                uint32_t diff = now - lastEdgeTime;
                
                // 20us 滤波，确保保留遥控器的短脉冲
                if (diff > 20) {
                    durations[edgeCount] = diff;
                    edgeCount++; 
                    lastEdgeTime = now;
                    lastState = currState;
                }
            }
        }
        
        if (edgeCount > 50) {
            // 遍历找 Sync
            for (int i = 0; i < edgeCount - 50; i++) {
                // Sync: High > 2000
                if (durations[i] > 2000 && durations[i] < 6000) {
                     
                     uint8_t buffer[8];
                     int bits = decodePulse(durations, i, edgeCount, buffer);
                     
                     // 只要解出数据就打印，但增加冷却时间防止刷屏
                     if (bits >= 16) { 
                         handleAction(buffer, bits);
                         
                         // === 防刷屏关键点 ===
                         // 成功识别一次后，强制冷却 500ms
                         lastPrint = millis(); 
                         return; 
                     }
                }
            }
        }
    }
}