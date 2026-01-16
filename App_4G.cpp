#include "App_4G.h"
#include <Arduino.h>

App4G My4G;

// [配置] 128KB 缓冲，PSRAM 必备
#define RX_BUF_SIZE (128 * 1024) 
static uint8_t* rxBuf = NULL;
static volatile int rxHead = 0;
static volatile int rxTail = 0;

static volatile bool g_needManualRead = false; 
static unsigned long g_lastPollTime = 0; 

// [新增] 状态机看门狗变量
static unsigned long g_lastRxTime = 0;

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
            
            // [修复] 即使在等待 OK，也要捕捉 PUSH 信号
            if (recv.indexOf("+MIPPUSH") != -1) {
                g_needManualRead = true;
                // 注意：不要清空 recv，因为可能 +MIPPUSH 和 OK 在同一个包里
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
    digitalWrite(PIN_4G_PWR, HIGH); 
    delay(500);
    
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    _serial4G->setRxBufferSize(8192); 
    delay(100);

    Serial.println("[4G] Syncing at 115200...");

    uint32_t foundBaud = 0;
    uint32_t scanBauds[] = {115200, 921600, 2000000}; 
    
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
    
    if (foundBaud != 115200) {
        Serial.println("[4G] Switching module back to 115200...");
        _serial4G->updateBaudRate(foundBaud);
        delay(50);
        _serial4G->println("AT+IPR=115200"); 
        delay(200);
        _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
        _serial4G->println("AT&W"); 
        delay(100);
    } else {
        _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    }

    delay(100);
    _serial4G->println("ATE0");       waitResponse("OK", 500); 
    _serial4G->println("AT+CSCLK=0"); waitResponse("OK", 500); 

    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);
    
    _has_pending_data = false;
    g_needManualRead = false;
    
    Serial.println("[4G] Power on and locked at 115200 OK.");
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
// [改进] 包含看门狗和调试输出的状态机
// =========================================================
static int dataBytesLeft = 0;
static char lenBuf[16]; 
static int lenBufIdx = 0;
static String headerMatch = "";

void App4G::process4GStream() {
    if (!rxBuf) return;

    // [看门狗] 如果状态机卡在非搜索状态超过 200ms 还没收到新字节，强制复位
    // 防止因为杂波或半截指令导致无法进入 ST_SEARCH，进而导致无法轮询
    if (g_st != ST_SEARCH && (millis() - g_lastRxTime > 200)) {
        // Serial.println("[4G] Watchdog: Reset state to SEARCH"); // 调试用
        g_st = ST_SEARCH;
        headerMatch = "";
    }

    while (_serial4G->available()) {
        char c = _serial4G->read();
        g_lastRxTime = millis(); // 更新收到数据的时间戳

        // [调试] 打开这个可以看底层到底收到了什么
        // if (isPrintable(c)) Serial.print(c); else Serial.printf("[%02X]", c);

        switch (g_st) {
            case ST_SEARCH: 
                // 更加宽容的匹配：只要遇到 M 就开始尝试，防止 + 号丢失
                if (c == '+' || c == 'M') {
                    headerMatch = String(c);
                    g_st = ST_MATCH_HEADER;
                }
                break;

            case ST_MATCH_HEADER:
                headerMatch += c;
                // 兼容 +MIPREAD 和 MIPREAD (无加号)
                if (headerMatch.endsWith("MIPREAD")) { 
                    g_st = ST_READ_LEN; 
                    lenBufIdx = 0;
                } else if (headerMatch.endsWith("MIPPUSH")) {
                    g_needManualRead = true;
                    g_st = ST_SEARCH;
                } else if (headerMatch.length() > 15) {
                    // 匹配串太长还没对上，重置
                    g_st = ST_SEARCH; 
                }
                break;

            case ST_READ_LEN:
                // 过滤掉头部可能残留的冒号或空格
                if (lenBufIdx == 0 && (c == ':' || c == ' ' || c == ',')) break;

                if (isDigit(c)) {
                    if (lenBufIdx < 10) lenBuf[lenBufIdx++] = c;
                } 
                else if (c == ',') {
                    // 遇到逗号，说明刚才读的是 ID，丢弃并重置
                    lenBufIdx = 0; 
                } 
                else if (c == '\r' || c == '\n') {
                    // 结束符
                    if (lenBufIdx > 0) {
                        lenBuf[lenBufIdx] = '\0';
                        dataBytesLeft = atoi(lenBuf);
                        Serial.printf("[4G Stream] Payload Len: %d\n", dataBytesLeft);

                        if (dataBytesLeft > 0) {
                            if (c == '\r') g_st = ST_SKIP_ID; // 借用 ST_SKIP_ID 等待 \n
                            else {
                                g_st = ST_READ_DATA; 
                                _has_pending_data = true;
                            }
                        } else {
                            g_st = ST_SEARCH; 
                        }
                    } else {
                        g_st = ST_SEARCH;
                    }
                }
                break;

            case ST_SKIP_ID: // 这里实际上是等待换行符 ST_WAIT_NEWLINE
                if (c == '\n') {
                    g_st = ST_READ_DATA;
                    _has_pending_data = true;
                } else if (!isSpace(c)) {
                    // 异常：没换行直接开始数据了？存入
                    int rxSize = (psramFound()) ? RX_BUF_SIZE : 16384; 
                    int next = (rxHead + 1) % rxSize;
                    if (next != rxTail) { rxBuf[rxHead] = (uint8_t)c; rxHead = next; }
                    dataBytesLeft--;
                    g_st = ST_READ_DATA;
                    _has_pending_data = true;
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
                
            default:
                g_st = ST_SEARCH;
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

int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t received = 0;
    
    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); 
        
        int b = popCache();
        if (b != -1) {
            buf[received++] = (uint8_t)b;
            start = millis(); 
            if (received == wantLen) break; 
        } else {
            // [修复] 轮询逻辑
            // 1. 如果状态机空闲 (ST_SEARCH) 
            // 2. 距离上次轮询超过 2000ms (防刷)
            if (g_st == ST_SEARCH && (millis() - g_lastPollTime > 2000)) {
                
                // 3. 触发条件：有PUSH标记 OR 缓冲有待处理数据 OR 距离上次太久(3s)
                if (g_needManualRead || _has_pending_data || (millis() - g_lastPollTime > 3000)) {
                     g_needManualRead = false;
                     // Serial.println("POLL");
                     _serial4G->println("AT+MIPREAD=1,3072");
                     g_lastPollTime = millis();
                }
            }
            vTaskDelay(1);
        }
    }
    return received;
}

bool App4G::isConnected() { return _modem && _modem->isNetworkConnected(); }
TinyGsmClient& App4G::getClient() { return *_client; }