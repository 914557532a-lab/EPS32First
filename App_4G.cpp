#include "App_4G.h"
#include <Arduino.h>

App4G My4G;

// 定义 RingBuffer 大小 (8KB 足够缓冲音频流)
#define RX_BUF_SIZE 8192
static uint8_t rxBuf[RX_BUF_SIZE];
static volatile int rxHead = 0;
static volatile int rxTail = 0;

void App4G::init() {
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
}

// [修改] 增加调试回显
bool App4G::waitResponse(const char* expected, uint32_t timeout) {
    unsigned long start = millis();
    String recv = "";
    while (millis() - start < timeout) {
        if (_serial4G->available()) {
            char c = _serial4G->read();
            
            // 【关键】打印 4G 模块的回复，方便你看到报错
            Serial.write(c); 
            
            recv += c;
            if (recv.indexOf(expected) != -1) return true;
        }
        vTaskDelay(1);
    }
    return false;
}

bool App4G::checkBaudrate(uint32_t baud) {
    _serial4G->updateBaudRate(baud);
    delay(50);
    while (_serial4G->available()) _serial4G->read(); 
    _serial4G->println("AT");
    return waitResponse("OK", 200);
}

void App4G::powerOn() {
    digitalWrite(PIN_4G_PWR, HIGH); delay(500);

    // 锁定 115200 波特率 (最稳)
    uint32_t targetBaud = 115200; 
    
    // 必须先 begin
    _serial4G->begin(targetBaud, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    delay(100);

    Serial.println("[4G] Trying to sync baudrate...");
    if (!checkBaudrate(targetBaud)) {
        if (checkBaudrate(460800)) {
            _serial4G->printf("AT+IPR=%d\r\n", targetBaud); delay(200);
        } else if (checkBaudrate(921600)) {
            _serial4G->printf("AT+IPR=%d\r\n", targetBaud); delay(200);
        } else {
            // 硬件复位
            Serial.println("[4G] Hard Resetting...");
            pinMode(PIN_4G_PWRKEY, OUTPUT);
            digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
            digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500);
            digitalWrite(PIN_4G_PWRKEY, HIGH); pinMode(PIN_4G_PWRKEY, INPUT);
            delay(10000);
        }
    }
    
    _serial4G->updateBaudRate(targetBaud);
    _serial4G->println("AT"); waitResponse("OK", 1000);
    _serial4G->println("ATE0"); waitResponse("OK", 500); 

    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);
}

// [修复] 真正的连接检查逻辑
bool App4G::connect(unsigned long timeout_ms) {
    unsigned long start = millis();
    Serial.print("[4G] Checking Network... ");
    
    while (millis() - start < timeout_ms) {
        // 使用 TinyGsm 检查网络注册状态
        if (_modem && _modem->isNetworkConnected()) {
            Serial.println("OK! (Connected)");
            return true;
        }
        
        Serial.print(".");
        vTaskDelay(1000); 
    }
    
    Serial.println(" Timeout!");
    return false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    rxHead = rxTail = 0; 
    g_st = ST_SEARCH;    
    
    // 1. 关闭可能存在的旧连接
    _serial4G->println("AT+MIPCLOSE=1"); 
    waitResponse("OK", 500);    

    // [删除] 既然之前报错 ERROR，说明模块不支持或不需要这个设置
    // 而且我们需要发送原始二进制，不设置反而更好！
    // _serial4G->println("AT+GTSET=\"IPRFMT\",2"); 
    // waitResponse("OK", 1000);

    // 2. 双重保险：检查是否有 IP，没有则激活
    // (防止 AT+MIPCALL=1 之后过一会掉线)
    _serial4G->println("AT+MIPCALL?");
    // 如果回复 +MIPCALL: 0 说明没 IP
    if (waitResponse("+MIPCALL: 0", 500)) {
         Serial.println("[4G] Reactivating IP...");
         _serial4G->println("AT+CGDCONT=1,\"IP\",\"cmnet\""); waitResponse("OK", 500);
         _serial4G->println("AT+MIPCALL=1"); 
         waitResponse("OK", 3000);
    }

    // 3. 发起连接
    _serial4G->printf("AT+MIPOPEN=1,0,\"%s\",%d,0\r\n", host, port);
    
    // 4. [核心修复] 智能等待连接结果
    // 只要收到 "+MIPOPEN: 1,1" 或者 "CONNECT" 都算成功
    unsigned long start = millis();
    while (millis() - start < 20000) {
        if (_serial4G->available()) {
            String line = _serial4G->readStringUntil('\n');
            line.trim();
            
            // 打印调试信息，让你看到发生了什么
            if (line.length() > 0) Serial.println("[4G RAW] " + line);
            
            // 判定成功条件：
            // 条件A: 出现 CONNECT (某些固件)
            // 条件B: 出现 +MIPOPEN: 1,1 (你的固件)
            if (line.indexOf("CONNECT") != -1) return true;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",1") != -1) return true;
            
            // 判定失败条件
            if (line.indexOf("ERROR") != -1) return false;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",0") != -1) return false;
        }
        vTaskDelay(10);
    }
    
    Serial.println("[4G] TCP Connect Timeout");
    return false;
}

void App4G::closeTCP() {
    _serial4G->println("AT+MIPCLOSE=1");
    waitResponse("OK", 2000);
}

bool App4G::sendData(const uint8_t* data, size_t len) {
    _serial4G->printf("AT+MIPSEND=1,%d\r\n", len);
    if (!waitResponse(">", 5000)) return false;
    _serial4G->write(data, len);
    return waitResponse("OK", 10000); 
}

bool App4G::sendData(uint8_t* data, size_t len) {
    return sendData((const uint8_t*)data, len);
}

// ==========================================================
// 核心：高性能状态机 (无 String, 纯字符匹配)
// ==========================================================
// 目标匹配模式: "+MIPRTCP:"
static const char* HEADER_MATCH = "+MIPRTCP:";
static int matchIdx = 0;
static char lenBuf[16]; 
static int lenBufIdx = 0;
static int dataBytesLeft = 0;

void App4G::process4GStream() {
    while (_serial4G->available()) {
        char c = _serial4G->read();

        switch (g_st) {
            case ST_SEARCH: 
                // 逐字匹配 "+MIPRTCP:"
                if (c == HEADER_MATCH[matchIdx]) {
                    matchIdx++;
                    if (HEADER_MATCH[matchIdx] == '\0') {
                        // 匹配成功!
                        g_st = ST_SKIP_ID;
                        matchIdx = 0;
                    }
                } else {
                    // 匹配失败，回退
                    if (c == HEADER_MATCH[0]) matchIdx = 1;
                    else matchIdx = 0;
                }
                break;

            case ST_SKIP_ID:
                // 跳过 " 1," 这样的ID部分，直到遇到逗号
                if (c == ',') {
                    g_st = ST_READ_LEN;
                    lenBufIdx = 0;
                    memset(lenBuf, 0, sizeof(lenBuf));
                }
                break;

            case ST_READ_LEN:
                // 读取长度数字，直到遇到下一个逗号
                if (c == ',') {
                    lenBuf[lenBufIdx] = '\0';
                    dataBytesLeft = atoi(lenBuf); // 解析长度
                    if (dataBytesLeft > 0) {
                        g_st = ST_READ_DATA;
                    } else {
                        g_st = ST_SEARCH; 
                    }
                } else if (isDigit(c)) {
                    if (lenBufIdx < 10) { // 防止溢出
                        lenBuf[lenBufIdx++] = c;
                    }
                }
                break;

            case ST_READ_DATA:
                if (dataBytesLeft > 0) {
                    // 存入环形缓冲区
                    int next = (rxHead + 1) % RX_BUF_SIZE;
                    if (next != rxTail) {
                        rxBuf[rxHead] = (uint8_t)c;
                        rxHead = next;
                    }
                    dataBytesLeft--;
                }
                
                if (dataBytesLeft == 0) {
                    g_st = ST_SEARCH; // 收完这一包，继续找下一包
                    matchIdx = 0;
                }
                break;
        }
    }
}

int App4G::popCache() {
    if (rxHead == rxTail) return -1;
    uint8_t c = rxBuf[rxTail];
    rxTail = (rxTail + 1) % RX_BUF_SIZE;
    return c;
}

int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t received = 0;
    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); // 搬运数据
        
        int b = popCache();
        if (b != -1) {
            buf[received++] = (uint8_t)b;
            start = millis(); // 收到数据刷新超时
            
            // 极速模式下，减少 vTaskDelay 频率
            if (received % 256 == 0) vTaskDelay(1); 
        } else {
            vTaskDelay(1); // 没数据时休息一下
        }
    }
    return received;
}

// 简单的 Getter 包装
bool App4G::isConnected() { return _modem && _modem->isNetworkConnected(); }
String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; }
void App4G::sendRawAT(String cmd) { _serial4G->println(cmd); }
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }