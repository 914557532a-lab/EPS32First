#include "App_4G.h"

App4G My4G;

// 【关键修复】将解析状态移到函数外面，作为全局静态变量
// 这样即使 readData 分多次调用，也能接上由于断点
static bool g_catchingHex = false;
static char g_highNibble = 0;
static bool g_hasHighNibble = false;

void App4G::init() {
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
    pinMode(PIN_4G_PWRKEY, INPUT); 

    // 1. 开启 R8 核动力 (256KB 接收缓存)
    // 只要 Arduino 选了 OPI PSRAM，这里绝对稳
    _serial4G->setRxBufferSize(262144); 
    
    // 2. 启动串口
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    
    // 稍微延时让内存分配完成
    delay(100);
    Serial.println("[4G] Init Done (RxBuf=256KB, Baud=115200).");
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
            // 这里不要打印，以免干扰 AT 指令判断
            resp += c;
            if (resp.indexOf(expected) != -1) return true;
        }
    }
    return false;
}

bool App4G::connect(unsigned long timeout_ms) {
    if (!_modem) return false;

    int csq = getSignalCSQ();
    Serial.printf("[4G] CSQ: %d\n", csq);
    if (csq == 99 || csq == 0) return false;

    Serial.println("[4G] Forcing Network Search...");
    _serial4G->println("AT+CFUN=1"); waitResponse("OK", 1000);
    _serial4G->println("AT+COPS=0"); waitResponse("OK", 3000);        

    Serial.println("[4G] Waiting for Network Reg...");
    bool isRegistered = false;
    unsigned long regStart = millis();
    while (millis() - regStart < 15000) {
        _serial4G->println("AT+CEREG?"); 
        if (waitResponse(",1", 500) || waitResponse(",5", 500)) {
            isRegistered = true; Serial.println("[4G] LTE Reg!"); break;
        }
        delay(500);
        _serial4G->println("AT+CGREG?");
        if (waitResponse(",1", 500) || waitResponse(",5", 500)) {
             isRegistered = true; Serial.println("[4G] 2G/3G Reg!"); break;
        }
        Serial.print("."); delay(1000);
    }
    
    _serial4G->println("AT+MIPCALL?");
    if (waitResponse("10.", 1000) || waitResponse("100.", 1000) || waitResponse("172.", 1000)) {
         Serial.println("[4G] Already connected.");
         _is_verified = true;
         return true;
    }

    Serial.println("[4G] Resetting MIPCALL...");
    _serial4G->println("AT+MIPCALL=0"); waitResponse("OK", 2000); 

    Serial.println("[4G] Fibocom Init...");
    _serial4G->println("AT+MIPCALL=1,\"" + _apn + "\"");
    if (waitResponse(".", 15000)) {
        _is_verified = true; Serial.println("[4G] CONNECT SUCCESS"); return true;
    }
    return false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    Serial.println("[4G] Cleaning up Socket 1...");
    _serial4G->println("AT+MIPCLOSE=1"); waitResponse("OK", 1000);    

    while(_serial4G->available()) _serial4G->read();
    delay(100); 

    String cmd = "AT+MIPOPEN=1,0,\"" + String(host) + "\"," + String(port) + ",0";
    Serial.println(">> " + cmd);
    _serial4G->println(cmd);
    
    if (waitResponse("+MIPOPEN: 1,1", 20000) || waitResponse("CONNECT", 20000) || waitResponse("OK", 20000)) {
        Serial.println("[4G] TCP Open OK"); return true;
    }
    Serial.println("[4G] TCP Open Failed"); return false;
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

// 【关键修改】使用全局静态变量 g_catchingHex 的读取函数
int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t totalReceived = 0;
    
    // 如果一次性要读很多，稍微多给点时间
    if (wantLen > 10000) timeout_ms += 20000;

    while (totalReceived < wantLen && (millis() - start < timeout_ms)) {
        if (_serial4G->available()) {
            char c = _serial4G->read();
            
            // 【开启上帝视角】
            // 因为我们有 256KB 大缓存，打印不会造成严重丢包
            // 我们必须看看模组到底发了什么！
            Serial.print(c); 

            // 如果还没进入 Hex 数据段，就检测头
            if (!g_catchingHex) {
                static String lineBuffer = ""; // 这个保持 static 没问题，只在检测头时用
                lineBuffer += c;
                if (lineBuffer.length() > 20) lineBuffer = lineBuffer.substring(1);
                
                // 只要看到 ,0, 或者 1,0, 就认为数据开始了
                if (lineBuffer.endsWith(",0,") || lineBuffer.endsWith("1,0,")) {
                    g_catchingHex = true;
                }
            } 
            // 如果已经在 Hex 数据段，直接解析
            else {
                if (isHexadecimalDigit(c)) {
                    if (!g_hasHighNibble) {
                        g_highNibble = hexCharToVal(c);
                        g_hasHighNibble = true;
                    } else {
                        buf[totalReceived++] = (g_highNibble << 4) | hexCharToVal(c);
                        g_hasHighNibble = false;
                        if (totalReceived >= wantLen) return totalReceived;
                    }
                } else {
                    // 如果遇到换行符，说明这一包数据传完了
                    // 重置状态，准备迎接下一个 +MIPRTCP 头
                    if (c == '\r' || c == '\n') {
                         g_catchingHex = false;
                         // 注意：这里不能清空 lineBuffer，因为那是局部静态变量
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