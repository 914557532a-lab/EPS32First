#include "App_4G.h"
#include <Arduino.h>

App4G My4G;

// [配置] 128KB 缓冲，PSRAM 必备
#define RX_BUF_SIZE (128 * 1024) 
static uint8_t* rxBuf = NULL;
static volatile int rxHead = 0;
static volatile int rxTail = 0;

static bool g_needManualRead = false; 

void App4G::init() {
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 

    if (psramFound()) {
        rxBuf = (uint8_t*)ps_malloc(RX_BUF_SIZE);
        Serial.printf("[4G] Buffer alloc in PSRAM: %d KB\n", RX_BUF_SIZE/1024);
    } else {
        rxBuf = (uint8_t*)malloc(16384);
        Serial.println("[4G] Warning: No PSRAM, alloc 16KB in SRAM");
    }
}

// Spy 版本的 waitResponse
bool App4G::waitResponse(const char* expected, uint32_t timeout) {
    unsigned long start = millis();
    String recv = "";
    recv.reserve(64); 
    
    while (millis() - start < timeout) {
        while (_serial4G->available()) {
            char c = _serial4G->read();
            recv += c;
            
            // 窃听 PUSH 通知
            if (!g_needManualRead && recv.indexOf("+MIPPUSH") != -1) {
                g_needManualRead = true;
            }

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
    
    for (int i = 0; i < 3; i++) {
        _serial4G->println("AT");
        delay(50);
        if (waitResponse("OK", 100)) return true;
    }
    return false;
}

void App4G::powerOn() {
    digitalWrite(PIN_4G_PWR, HIGH); delay(500);
    
    uint32_t targetBauds[] = {2000000, 921600};
    uint32_t finalBaud = 0;

    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    _serial4G->setRxBufferSize(4096); 
    delay(100);

    Serial.println("[4G] Syncing...");

    uint32_t foundBaud = 0;
    uint32_t scanBauds[] = {115200, 921600, 2000000, 1500000, 3000000};
    
    for (uint32_t b : scanBauds) {
        if (checkBaudrate(b)) { foundBaud = b; break; }
    }

    if (foundBaud == 0) {
        Serial.println("[4G] Sync failed, Hard Resetting...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500);
        digitalWrite(PIN_4G_PWRKEY, HIGH); pinMode(PIN_4G_PWRKEY, INPUT);
        delay(6000); 
        foundBaud = 115200;
    }

    Serial.printf("[4G] Found module at %d\n", foundBaud);
    
    bool success = false;
    for (uint32_t t : targetBauds) {
        if (foundBaud == t) {
            finalBaud = t;
            success = true;
            break;
        }
        
        Serial.printf("[4G] Switching to %d...\n", t);
        _serial4G->printf("AT+IPR=%d\r\n", t);
        delay(200); 
        _serial4G->begin(t, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX); 
        delay(100);

        if (checkBaudrate(t)) {
            finalBaud = t;
            success = true;
            _serial4G->println("AT&W"); 
            Serial.println(" -> OK!");
            break;
        } else {
            Serial.println(" -> Failed, reverting...");
            _serial4G->begin(foundBaud, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
            delay(100);
            checkBaudrate(foundBaud); 
        }
    }
    
    if (!success) {
        Serial.println("[4G] Fallback to 921600.");
        finalBaud = 921600;
        _serial4G->printf("AT+IPR=%d\r\n", finalBaud);
        delay(200);
        _serial4G->begin(finalBaud, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    }

    delay(100);
    bool online = false;
    for(int i=0; i<5; i++) {
        _serial4G->println("AT");
        if(waitResponse("OK", 500)) { online = true; break; }
        delay(500);
    }

    if (online) Serial.printf("[4G] Online at %d baud.\n", finalBaud);
    else Serial.println("[4G] FATAL: Module unresponsive.");

    _serial4G->println("ATE0"); waitResponse("OK", 500); 
    _serial4G->println("AT+CSCLK=0"); waitResponse("OK", 500);

    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);
    
    _has_pending_data = false;
    g_needManualRead = false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    rxHead = rxTail = 0; 
    g_st = ST_SEARCH;    
    g_needManualRead = false;
    _has_pending_data = false;
    
    _serial4G->println("AT+MIPCLOSE=1"); waitResponse("OK", 500);    
    
    _serial4G->println("AT+MIPCALL?");
    if (waitResponse("+MIPCALL: 0", 500)) {
         _serial4G->println("AT+CGDCONT=1,\"IP\",\"cmnet\""); waitResponse("OK", 500);
         _serial4G->println("AT+MIPCALL=1"); waitResponse("OK", 3000);
    }

    _serial4G->printf("AT+MIPOPEN=1,0,\"%s\",%d,0\r\n", host, port);
    
    unsigned long start = millis();
    while (millis() - start < 15000) {
        if (_serial4G->available()) {
            String line = _serial4G->readStringUntil('\n');
            line.trim();
            if (line.length() > 0) Serial.println("[4G RAW] " + line);
            
            if (line.indexOf("CONNECT") != -1) return true;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",1") != -1) return true;
            if (line.indexOf("ERROR") != -1) return false;
            if (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",0") != -1) return false;
        }
        vTaskDelay(10);
    }
    return false;
}

void App4G::closeTCP() {
    _serial4G->println("AT+MIPCLOSE=1");
    waitResponse("OK", 1000);
}

bool App4G::sendData(const uint8_t* data, size_t len) {
    _serial4G->printf("AT+MIPSEND=1,%d\r\n", len);
    if (!waitResponse(">", 3000)) return false;
    _serial4G->write(data, len);
    return waitResponse("OK", 5000); 
}

bool App4G::sendData(uint8_t* data, size_t len) {
    return sendData((const uint8_t*)data, len);
}

// 状态机变量
static int matchIdx = 0;
static int dataBytesLeft = 0;
static char lenBuf[16]; 
static int lenBufIdx = 0;

void App4G::resetParser() {
    g_st = ST_SEARCH;
    matchIdx = 0;
}

void App4G::process4GStream() {
    if (!rxBuf) return;

    while (_serial4G->available()) {
        char c = _serial4G->read();

        switch (g_st) {
            case ST_SEARCH: 
                if (c == '+') {
                    String header = "+";
                    uint32_t t = millis();
                    
                    // [修复] 增加预读超时时间到 50ms
                    // 确保能读完 "+MIPPUSH:"，防止因串口数据分段到达而匹配失败
                    while(millis() - t < 50) { 
                        if(_serial4G->available()) {
                            char n = _serial4G->read();
                            header += n;
                            if (header.length() >= 9) break; 
                        }
                    }
                    
                    if (header.startsWith("+MIPRTCP:")) {
                        g_st = ST_SKIP_ID; 
                    } else if (header.startsWith("+MIPREAD:")) {
                        g_st = ST_READ_LEN; 
                        lenBufIdx = 0; memset(lenBuf, 0, sizeof(lenBuf));
                    } else if (header.startsWith("+MIPPUSH:")) {
                        g_needManualRead = true; 
                        g_st = ST_SEARCH;
                    } else {
                        // 没匹配上，重新开始搜索
                        g_st = ST_SEARCH;
                    }
                }
                break;

            case ST_SKIP_ID:
                if (c == ',') {
                    g_st = ST_READ_LEN;
                    lenBufIdx = 0; memset(lenBuf, 0, sizeof(lenBuf));
                }
                break;

            case ST_READ_LEN:
                if (c == ',' || c == '\r' || c == '\n') { 
                    lenBuf[lenBufIdx] = '\0';
                    dataBytesLeft = atoi(lenBuf);
                    if (dataBytesLeft > 0) {
                        g_st = ST_READ_DATA;
                        _has_pending_data = true; 
                    } else {
                        _has_pending_data = false;
                        g_st = ST_SEARCH; 
                    }
                } else if (isDigit(c)) {
                    if (lenBufIdx < 10) lenBuf[lenBufIdx++] = c;
                }
                break;

            case ST_READ_DATA:
                if (dataBytesLeft > 0) {
                    int rxSize = (psramFound()) ? RX_BUF_SIZE : 16384; 
                    int next = (rxHead + 1) % rxSize;
                    if (next != rxTail) {
                        rxBuf[rxHead] = (uint8_t)c;
                        rxHead = next;
                    }
                    dataBytesLeft--;
                }
                if (dataBytesLeft == 0) g_st = ST_SEARCH; 
                break;
        }
    }
}

int App4G::popCache() {
    if (!rxBuf) return -1;
    if (rxHead == rxTail) return -1;
    
    int rxSize = (psramFound()) ? RX_BUF_SIZE : 16384;
    uint8_t c = rxBuf[rxTail];
    rxTail = (rxTail + 1) % rxSize;
    return c;
}

int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t received = 0;
    
    // 初始触发
    if ((rxHead == rxTail) && (g_needManualRead || _has_pending_data)) {
        g_needManualRead = false;
        _serial4G->println("AT+MIPREAD=1,1500");
    }

    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); 
        
        int b = popCache();
        if (b != -1) {
            buf[received++] = (uint8_t)b;
            start = millis(); 
            if (received == wantLen) break; 
        } else {
            // 循环内触发
            if (g_needManualRead || _has_pending_data) {
                 g_needManualRead = false;
                 _serial4G->println("AT+MIPREAD=1,1500");
                 start = millis(); 
            }
            delayMicroseconds(100);
        }
    }
    return received;
}

bool App4G::isConnected() { return _modem && _modem->isNetworkConnected(); }
TinyGsmClient& App4G::getClient() { return *_client; }