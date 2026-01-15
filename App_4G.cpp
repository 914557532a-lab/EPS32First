#include "App_4G.h"

App4G My4G;

// --- 二进制环形缓冲区 (仓库) ---
#define BIN_CACHE_SIZE (128 * 1024) 
static uint8_t* g_binCache = nullptr;
static volatile size_t g_writeIdx = 0;
static volatile size_t g_readIdx = 0;

// --- 解析状态机 ---
enum StreamState { ST_SEARCH, ST_SKIP_COMMAS, ST_READ_HEX };
static StreamState g_st = ST_SEARCH;
static String g_rawBuf = ""; 
static int g_commaCount = 0;
static uint8_t g_hi = 0;
static bool g_hasHi = false;

void pushCache(uint8_t b) {
    if (!g_binCache) return;
    size_t next = (g_writeIdx + 1) % BIN_CACHE_SIZE;
    if (next != g_readIdx) { g_binCache[g_writeIdx] = b; g_writeIdx = next; }
}

int popCache() {
    if (g_writeIdx == g_readIdx) return -1;
    uint8_t b = g_binCache[g_readIdx];
    g_readIdx = (g_readIdx + 1) % BIN_CACHE_SIZE;
    return b;
}

// 高性能拆箱员：解析 +MIPRTCP 快递并存入仓库
void process4GStream() {
    HardwareSerial* s = My4G.getClientSerial();
    if (!s) return;
    while (s->available()) {
        uint8_t c = s->read();
        
        // 【调试】非读取数据阶段回显所有字符，让你看到 URC 包头
        if (g_st != ST_READ_HEX) Serial.write(c);

        switch (g_st) {
            case ST_SEARCH:
                g_rawBuf += (char)c;
                if (g_rawBuf.length() > 30) g_rawBuf.remove(0, 1);
                if (g_rawBuf.endsWith("+MIPRTCP:") || g_rawBuf.endsWith("+MND:")) {
                    g_st = ST_SKIP_COMMAS; g_commaCount = 0; g_rawBuf = "";
                }
                break;
            case ST_SKIP_COMMAS:
                if (c == ',') {
                    g_commaCount++;
                    if (g_commaCount >= 2) { g_st = ST_READ_HEX; g_hasHi = false; }
                }
                break;
            case ST_READ_HEX:
                if (isHexadecimalDigit(c)) {
                    uint8_t val = (c >= 'A') ? (c - 'A' + 10) : (c >= 'a' ? c - 'a' + 10 : c - '0');
                    if (!g_hasHi) { g_hi = val; g_hasHi = true; }
                    else { pushCache((g_hi << 4) | val); g_hasHi = false; }
                } else if (c == '\r' || c == '\n' || c == '+') {
                    g_st = ST_SEARCH;
                    if (c == '+') g_rawBuf = "+";
                }
                break;
        }
    }
}

void App4G::init() {
    if (!g_binCache) g_binCache = (uint8_t*)malloc(BIN_CACHE_SIZE);
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
    pinMode(PIN_4G_PWRKEY, INPUT); 
    _serial4G->setRxBufferSize(262144); 
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
}

bool App4G::checkBaudrate(uint32_t baud) {
    _serial4G->updateBaudRate(baud); 
    delay(100);
    while(_serial4G->available()) _serial4G->read();
    for (int i = 0; i < 3; i++) {
        _serial4G->println("AT");
        unsigned long start = millis();
        String resp = "";
        while(millis() - start < 500) {
            if (_serial4G->available()) resp += (char)_serial4G->read();
            if (resp.indexOf("OK") != -1) return true;
        }
    }
    return false;
}

void App4G::powerOn() {
    digitalWrite(PIN_4G_PWR, HIGH); delay(500);
    // 降速至 460800 以保证在杜邦线连接下 100% 稳定
    uint32_t targetBaud = 460800;
    if (!checkBaudrate(targetBaud)) {
        if (!checkBaudrate(921600)) {
            if (!checkBaudrate(115200)) {
                pinMode(PIN_4G_PWRKEY, OUTPUT);
                digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
                digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500);
                digitalWrite(PIN_4G_PWRKEY, HIGH); pinMode(PIN_4G_PWRKEY, INPUT);
                delay(10000);
                checkBaudrate(115200);
            }
        }
    }
    _serial4G->printf("AT+IPR=%d\r\n", targetBaud); delay(200);
    _serial4G->updateBaudRate(targetBaud);
    _serial4G->println("AT"); waitResponse("OK", 1000);
    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);
}

bool App4G::waitResponse(String expected, int timeout) {
    unsigned long start = millis();
    while (millis() - start < timeout) {
        process4GStream(); 
        if (g_rawBuf.indexOf(expected) != -1) { g_rawBuf = ""; return true; }
        vTaskDelay(5);
    }
    return false;
}

int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t received = 0;
    while (received < wantLen && (millis() - start < timeout_ms)) {
        process4GStream(); 
        int b = popCache();
        if (b != -1) { buf[received++] = (uint8_t)b; start = millis(); }
        else { vTaskDelay(1); }
    }
    return received;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    g_writeIdx = 0; g_readIdx = 0; g_rawBuf = ""; g_st = ST_SEARCH;
    _serial4G->println("AT+MIPCLOSE=1"); waitResponse("OK", 500);    
    _serial4G->printf("AT+MIPOPEN=1,0,\"%s\",%d,0\r\n", host, port);
    return waitResponse("CONNECT", 20000) || waitResponse("OK", 20000);
}

bool App4G::sendData(const uint8_t* data, size_t len) {
    _serial4G->printf("AT+MIPSEND=1,%d\r\n", len);
    if (!waitResponse(">", 3000)) return false; 
    _serial4G->write(data, len);
    return waitResponse("OK", 8000);
}

void App4G::closeTCP() { _serial4G->println("AT+MIPCLOSE=1"); waitResponse("OK", 1000); }
bool App4G::isConnected() { return _is_verified; }
String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; } 
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }
bool App4G::connect(unsigned long t) { 
    _serial4G->println("AT+MIPCALL=1,\"" + _apn + "\"");
    if(waitResponse(".", 10000)) { _is_verified = true; return true; }
    return false; 
}
void App4G::sendRawAT(String cmd) { _serial4G->println(cmd); waitResponse("OK", 2000); }