#include "App_4G.h"
#include <Arduino.h>

App4G My4G;

// [配置] 128KB 缓冲，PSRAM 必备
#define RX_BUF_SIZE (128 * 1024) 
static uint8_t* rxBuf = NULL;
static volatile int rxHead = 0;
static volatile int rxTail = 0;

static volatile bool g_needManualRead = false; 

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

// 优化的 waitResponse
bool App4G::waitResponse(const char* expected, uint32_t timeout) {
    unsigned long start = millis();
    String recv = "";
    recv.reserve(64); 
    
    while (millis() - start < timeout) {
        while (_serial4G->available()) {
            char c = _serial4G->read();
            recv += c;
            
            // 记录 PUSH 通知
            if (recv.indexOf("+MIPPUSH") != -1) {
                g_needManualRead = true;
                recv = ""; 
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
        if (waitResponse("OK", 150)) return true;
    }
    return false;
}

void App4G::powerOn() {
    digitalWrite(PIN_4G_PWR, HIGH); delay(500);
    
    uint32_t targetBauds[] = {2000000, 921600};
    uint32_t finalBaud = 0;

    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    _serial4G->setRxBufferSize(8192); // 加大驱动层缓冲
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
        if (foundBaud == t) { finalBaud = t; success = true; break; }
        
        Serial.printf("[4G] Switching to %d...\n", t);
        _serial4G->printf("AT+IPR=%d\r\n", t);
        delay(200); 
        _serial4G->begin(t, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX); 
        delay(100);

        if (checkBaudrate(t)) {
            finalBaud = t;
            success = true;
            _serial4G->println("AT&W"); 
            break;
        } else {
            _serial4G->begin(foundBaud, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
            delay(100);
            checkBaudrate(foundBaud); 
        }
    }
    
    if (!success) {
        finalBaud = foundBaud;
        _serial4G->begin(finalBaud, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    }

    delay(100);
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
         _serial4G->println("AT+MIPCALL=1"); waitResponse("OK", 5000);
    }

    _serial4G->printf("AT+MIPOPEN=1,0,\"%s\",%d,0\r\n", host, port);
    
    unsigned long start = millis();
    while (millis() - start < 15000) {
        if (_serial4G->available()) {
            String line = _serial4G->readStringUntil('\n');
            line.trim();
            if (line.indexOf("CONNECT") != -1 || (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",1") != -1)) return true;
            if (line.indexOf("ERROR") != -1 || (line.indexOf("+MIPOPEN:") != -1 && line.indexOf(",0") != -1)) return false;
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

bool App4G::sendData(uint8_t* data, size_t len) { return sendData((const uint8_t*)data, len); }

// =========================================================
// [改进] 异步解析状态机 - 杜绝数据遗漏
// =========================================================
static int dataBytesLeft = 0;
static char lenBuf[16]; 
static int lenBufIdx = 0;
static String headerMatch = "";

void App4G::process4GStream() {
    if (!rxBuf) return;

    while (_serial4G->available()) {
        char c = _serial4G->read();

        switch (g_st) {
            case ST_SEARCH: 
                if (c == '+') {
                    headerMatch = "+";
                    g_st = ST_MATCH_HEADER;
                }
                break;

            case ST_MATCH_HEADER:
                headerMatch += c;
                if (headerMatch == "+MIPREAD:") {
                    g_st = ST_SKIP_ID;
                } else if (headerMatch == "+MIPPUSH:") {
                    g_needManualRead = true;
                    g_st = ST_SEARCH;
                } else if (headerMatch.length() > 10) {
                    g_st = ST_SEARCH; 
                }
                break;

            case ST_SKIP_ID:
                if (c == ',') {
                    g_st = ST_READ_LEN;
                    lenBufIdx = 0;
                }
                break;

            case ST_READ_LEN:
                if (c == '\r' || c == '\n' || c == ',') { 
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
    if (!rxBuf || rxHead == rxTail) return -1;
    int rxSize = (psramFound()) ? RX_BUF_SIZE : 16384;
    uint8_t c = rxBuf[rxTail];
    rxTail = (rxTail + 1) % rxSize;
    return c;
}

// [核心优化] 强制拉取机制
int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    unsigned long lastPoll = 0;
    size_t received = 0;
    
    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); 
        
        int b = popCache();
        if (b != -1) {
            buf[received++] = (uint8_t)b;
            start = millis(); 
            if (received == wantLen) break; 
        } else {
            // 如果缓存空了，且距离上次拉取超过 300ms，则主动发送请求
            if (g_needManualRead || _has_pending_data || (millis() - lastPoll > 300)) {
                 g_needManualRead = false;
                 _serial4G->println("AT+MIPREAD=1,3072");
                 lastPoll = millis();
            }
            vTaskDelay(1);
        }
    }
    return received;
}

bool App4G::isConnected() { return _modem && _modem->isNetworkConnected(); }
TinyGsmClient& App4G::getClient() { return *_client; }