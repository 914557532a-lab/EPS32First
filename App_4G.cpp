#include "App_4G.h"

App4G My4G;

// 定义缓冲区
uint8_t _internalBuffer[2048]; 
int _internalBufferHead = 0;
int _internalBufferTail = 0;

void App4G::init() {
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
    pinMode(PIN_4G_PWRKEY, INPUT); 

    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    Serial.println("[4G] Init Done.");
}

void App4G::powerOn() {
    Serial.println("[4G] Starting Power Sequence...");
    digitalWrite(PIN_4G_PWR, HIGH);
    delay(500);
    Serial.println("[4G] Waiting for boot...");
    delay(5000);

    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);

    bool alive = false;
    for(int i=0; i<10; i++) {
        _serial4G->println("AT");
        if (waitResponse("OK", 500)) { 
            alive = true;
            Serial.println("\n[4G] Module Alive!");
            break;
        }
        delay(500);
    }

    if (!alive) {
        Serial.println("\n[4G] No Response. Pulse PowerKey...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500);
        digitalWrite(PIN_4G_PWRKEY, HIGH); 
        pinMode(PIN_4G_PWRKEY, INPUT);   
        delay(5000);
    }
    
    // 开机强制开启射频
    Serial.println("[4G] Set Full Functionality...");
    _serial4G->println("AT+CFUN=1");
    waitResponse("OK", 2000);
}

void App4G::sendRawAT(String cmd) {
    if (!_serial4G) return;
    while(_serial4G->available()) _serial4G->read(); 
    Serial.print("[CMD] "); Serial.println(cmd);
    _serial4G->println(cmd);
    waitResponse("OK", 1000);
}

bool App4G::waitResponse(String expected, int timeout) {
    String resp = "";
    unsigned long start = millis();
    while (millis() - start < timeout) {
        vTaskDelay(pdMS_TO_TICKS(5)); 
        while (_serial4G->available()) {
            char c = _serial4G->read();
            Serial.print(c); 
            resp += c;
            if (resp.indexOf(expected) != -1) return true;
        }
    }
    return false;
}

// 包含强制唤醒和注册等待的连接逻辑
bool App4G::connect(unsigned long timeout_ms) {
    if (!_modem) return false;

    int csq = getSignalCSQ();
    Serial.printf("[4G] CSQ: %d\n", csq);
    if (csq == 99 || csq == 0) return false;

    // 强制唤醒射频 & 自动搜网
    Serial.println("[4G] Forcing Network Search...");
    _serial4G->println("AT+CFUN=1"); 
    waitResponse("OK", 1000);
    _serial4G->println("AT+COPS=0"); 
    waitResponse("OK", 3000);        

    // 等待注册
    Serial.println("[4G] Waiting for Network Reg...");
    bool isRegistered = false;
    unsigned long regStart = millis();
    
    while (millis() - regStart < 15000) {
        _serial4G->println("AT+CEREG?"); 
        if (waitResponse(",1", 500) || waitResponse(",5", 500)) {
            isRegistered = true;
            Serial.println("[4G] 4G/LTE Registered!");
            break;
        }
        delay(500);
        _serial4G->println("AT+CGREG?");
        if (waitResponse(",1", 500) || waitResponse(",5", 500)) {
             isRegistered = true;
             Serial.println("[4G] 2G/3G Registered!");
             break;
        }
        Serial.print(".");
        delay(1000);
    }
    
    // 检查是否已有 IP
    _serial4G->println("AT+MIPCALL?");
    if (waitResponse("10.", 1000) || waitResponse("100.", 1000) || waitResponse("172.", 1000)) {
         Serial.println("[4G] Already connected.");
         _is_verified = true;
         return true;
    }

    // 复位并拨号
    Serial.println("[4G] Resetting MIPCALL...");
    _serial4G->println("AT+MIPCALL=0");
    waitResponse("OK", 2000); 

    Serial.println("[4G] Fibocom Init...");
    _serial4G->println("AT+MIPCALL=1,\"" + _apn + "\"");
    
    if (waitResponse(".", 15000)) {
        _is_verified = true;
        Serial.println("[4G] >>> CONNECT SUCCESS (Fibocom) <<<");
        return true;
    }

    Serial.println("[4G] Connect Failed.");
    return false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    Serial.println("[4G] Cleaning up Socket 1...");
    _serial4G->println("AT+MIPCLOSE=1");
    waitResponse("ERROR", 1000); 
    waitResponse("OK", 1000);    

    while(_serial4G->available()) _serial4G->read();
    delay(100); 

    // 使用正确的指令格式: AT+MIPOPEN=1,0,"host",port,0
    String cmd = "AT+MIPOPEN=1,0,\"" + String(host) + "\"," + String(port) + ",0";
    Serial.println(">> " + cmd);
    _serial4G->println(cmd);
    
    // 等待连接成功 (+MIPOPEN: 1,1)
    if (waitResponse("+MIPOPEN: 1,1", 20000) || waitResponse("CONNECT", 20000) || waitResponse("OK", 20000)) {
        Serial.println("[4G] TCP Open OK");
        return true;
    }
    
    Serial.println("[4G] TCP Open Failed");
    return false;
}

bool App4G::sendData(const uint8_t* data, size_t len) {
    _serial4G->printf("AT+MIPSEND=1,%d\r\n", len);
    if (!waitResponse(">", 3000)) return false; 
    _serial4G->write(data, len);
    return waitResponse("OK", 8000) || waitResponse("SEND OK", 8000);
}

uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// 读取数据：解析 +MIPRTCP 推送
int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t totalReceived = 0;
    bool catchingHex = false; 
    char highNibble = 0;      
    bool hasHighNibble = false; 

    while (totalReceived < wantLen && (millis() - start < timeout_ms)) {
        if (_serial4G->available()) {
            char c = _serial4G->read();
            static String lineBuffer = "";
            if (!catchingHex) {
                lineBuffer += c;
                if (lineBuffer.length() > 20) lineBuffer = lineBuffer.substring(1);
                // 只要看到 ,0, 就认为数据开始了 (适配 +MIPRTCP: 1,0,...)
                if (lineBuffer.endsWith(",0,")) catchingHex = true;
            } else {
                if (isHexadecimalDigit(c)) {
                    if (!hasHighNibble) {
                        highNibble = hexCharToVal(c);
                        hasHighNibble = true;
                    } else {
                        buf[totalReceived++] = (highNibble << 4) | hexCharToVal(c);
                        hasHighNibble = false;
                        if (totalReceived >= wantLen) return totalReceived;
                    }
                } else {
                    if (c == '\r' || c == '\n') {
                         catchingHex = false;
                         lineBuffer = "";
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return totalReceived;
}

void App4G::closeTCP() {
    _serial4G->println("AT+MIPCLOSE=1");
    waitResponse("OK", 1000);
}
bool App4G::isConnected() { return _is_verified; }
String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; } 
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }
bool App4G::checkIP_manual() { return true; }