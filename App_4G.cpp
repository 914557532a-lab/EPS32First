#include "App_4G.h"

App4G My4G;

// --- 全局静态变量 ---
static bool g_catchingHex = false;
static char g_highNibble = 0;
static bool g_hasHighNibble = false;
static String g_rxBuffer = ""; // 防丢缓冲区

void App4G::init() {
    // 1. 仅配置引脚，不尝试通信，因为还没上电
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); 
    pinMode(PIN_4G_PWRKEY, INPUT); 

    // 2. 开启大缓存，准备迎接高速数据
    _serial4G->setRxBufferSize(262144); 
    
    // 默认先用 115200 启动串口，等 PowerOn 之后再协商
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);
    
    delay(100);
    Serial.println("[4G] Init Pins Done.");
}

// 替换 App_4G.cpp 中的 checkBaudrate 函数
bool App4G::checkBaudrate(uint32_t baud) {
    Serial.printf("[4G] Probing baudrate: %d ... ", baud);
    _serial4G->updateBaudRate(baud); 
    delay(50); // 给串口一点时间切换

    // 清空缓存
    while(_serial4G->available()) _serial4G->read();
    
    // 尝试发送 3 次 AT，防止一次发丢
    for (int i = 0; i < 3; i++) {
        _serial4G->println("AT");
        
        // 【关键修改】等待时间延长到 500ms
        unsigned long start = millis();
        String resp = "";
        while(millis() - start < 500) { 
            if (_serial4G->available()) {
                resp += (char)_serial4G->read();
            }
            // 一旦检测到 OK 立刻返回
            if (resp.indexOf("OK") != -1) {
                Serial.println("PASS");
                return true;
            }
        }
        delay(100); // 没收到就稍微歇一下再重试
    }
    
    Serial.println("FAIL");
    return false;
}

void App4G::powerOn() {
    Serial.println("[4G] Powering On Sequence...");
    
    // 1. 硬件开机动作
    digitalWrite(PIN_4G_PWR, HIGH); // 主电源
    delay(500);
    
    // 2. 智能波特率与开机检测
    Serial.println("[4G] Checking Module State & Baudrate...");
    
    bool alive = false;
    uint32_t currentBaud = 115200;

    // 尝试 A: 也许模块已经是 921600 了？
    if (checkBaudrate(921600)) {
        alive = true;
        currentBaud = 921600;
        Serial.println("[4G] Module found at 921600!");
    } 
    // 尝试 B: 也许模块还在 115200？
    else if (checkBaudrate(115200)) {
        alive = true;
        currentBaud = 115200;
        Serial.println("[4G] Module found at 115200.");
    }

    // 3. 如果都不通，说明模块可能没开机，尝试脉冲开机
    if (!alive) {
        Serial.println("[4G] No response. Pulsing PowerKey...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500); // 长按开机
        digitalWrite(PIN_4G_PWRKEY, HIGH); 
        pinMode(PIN_4G_PWRKEY, INPUT);   
        
        Serial.println("[4G] Waiting for Boot (10s)...");
        delay(10000); // 等待系统启动

        // 再次检测
        if (checkBaudrate(921600)) {
            alive = true; currentBaud = 921600;
        } else if (checkBaudrate(115200)) {
            alive = true; currentBaud = 115200;
        }
    }

    if (!alive) {
        Serial.println("[4G] ERROR: Module not responding! Check Hardware.");
        return; // 放弃
    }

    // 4. 统一升级到 921600
    if (currentBaud == 115200) {
        Serial.println("[4G] Upgrading Baudrate to 921600...");
        _serial4G->println("AT+IPR=921600"); 
        delay(500);
        _serial4G->println("AT&W"); // 保存配置
        delay(500);
        
        // ESP32 切到 921600
        _serial4G->updateBaudRate(921600);
        delay(100);
    }

    // 5. 最终确认
    _serial4G->println("AT");
    if (waitResponse("OK", 1000)) {
        Serial.println("[4G] Online at 921600 bps.");
    } else {
        Serial.println("[4G] Warning: Sync lost after baud change.");
    }

    // 6. 初始化 TinyGSM
    if (_modem == nullptr) _modem = new TinyGsm(*_serial4G);
    if (_client == nullptr) _client = new TinyGsmClient(*_modem);

    _serial4G->println("AT+CFUN=1");
    waitResponse("OK", 2000);
}

void App4G::sendRawAT(String cmd) {
    if (!_serial4G) return;
    while(_serial4G->available()) _serial4G->read(); 
    Serial.print("[CMD] "); Serial.println(cmd);
    _serial4G->println(cmd);
    // 这里会调用 waitResponse，它现在会打印返回内容
    waitResponse("OK", 2000);
}

// 【关键修复】现在这个函数会打印数据了 (Verbose Mode)
bool App4G::waitResponse(String expected, int timeout) {
    unsigned long start = millis();
    while (millis() - start < timeout) {
        vTaskDelay(pdMS_TO_TICKS(5)); 
        
        while (_serial4G->available()) {
            char c = _serial4G->read();
            g_rxBuffer += c;
            Serial.print(c); // <--- 【这里】开启透视眼，让你看到模块的回复！
        }

        int idx = g_rxBuffer.indexOf(expected);
        if (idx != -1) {
            g_rxBuffer = g_rxBuffer.substring(idx + expected.length());
            return true;
        }
    }
    return false;
}

// 下面的函数保持之前的“防丢包 + 单声道修复”逻辑
// 省略部分重复代码，直接复制之前的 connect, connectTCP, sendData, readData 等
// 只要确保 waitResponse 是上面这个带 Serial.print 的版本即可

bool App4G::connect(unsigned long timeout_ms) {
    if (!_modem) return false;
    int csq = getSignalCSQ();
    if (csq == 99 || csq == 0) return false;

    // Fibocom 专用的拨号检查
    _serial4G->println("AT+MIPCALL?");
    // 只要看到 IP 地址就认为连上了
    if (waitResponse("10.", 1000) || waitResponse("172.", 1000) || waitResponse("100.", 1000)) {
         _is_verified = true; return true;
    }

    _serial4G->println("AT+MIPCALL=1,\"" + _apn + "\"");
    if (waitResponse(".", 10000)) { // 等待 IP 地址返回
        _is_verified = true; return true;
    }
    return false;
}

bool App4G::connectTCP(const char* host, uint16_t port) {
    g_rxBuffer = ""; 
    g_catchingHex = false;
    
    _serial4G->println("AT+MIPCLOSE=1"); waitResponse("OK", 500);    

    String cmd = "AT+MIPOPEN=1,0,\"" + String(host) + "\"," + String(port) + ",0";
    Serial.println(">> " + cmd);
    _serial4G->println(cmd);
    
    return waitResponse("CONNECT", 20000) || waitResponse("OK", 20000);
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

// [App_4G.cpp] 修复后的 readData 函数
int App4G::readData(uint8_t* buf, size_t wantLen, uint32_t timeout_ms) {
    unsigned long start = millis();
    size_t totalReceived = 0;
    uint32_t loopCount = 0;

    while (totalReceived < wantLen && (millis() - start < timeout_ms)) {
        // 1. 喂狗，防止 WDT
        if (++loopCount > 500) { loopCount = 0; vTaskDelay(1); }

        // 2. 统一获取下一个字符（优先缓冲区，再串口）
        int c = -1;
        if (g_rxBuffer.length() > 0) {
            c = (uint8_t)g_rxBuffer.charAt(0);
            g_rxBuffer = g_rxBuffer.substring(1);
        } else if (_serial4G->available()) {
            c = _serial4G->read();
        }

        if (c != -1) {
            char ch = (char)c;

            if (!g_catchingHex) {
                static String headerBuf = "";
                headerBuf += ch;
                if (headerBuf.length() > 40) headerBuf = headerBuf.substring(1);

                // 检测 Fibocom 模块的数据起始头
                if (headerBuf.endsWith("+MIPRTCP:") || headerBuf.endsWith("+MND:")) {
                    Serial.println("\n[4G] Sync Header Found! Skipping parameters...");
                    
                    // 【修复核心】在这里连续读取，直到跳过 2 个逗号进入数据区
                    int commaCount = 0;
                    unsigned long skipStart = millis();
                    while (commaCount < 2 && (millis() - skipStart < 500)) {
                        int nextC = -1;
                        // 同样要先检查缓冲区
                        if (g_rxBuffer.length() > 0) {
                            nextC = (uint8_t)g_rxBuffer.charAt(0);
                            g_rxBuffer = g_rxBuffer.substring(1);
                        } else if (_serial4G->available()) {
                            nextC = _serial4G->read();
                        }

                        if (nextC != -1) {
                            if ((char)nextC == ',') commaCount++;
                        } else {
                            vTaskDelay(1);
                        }
                    }
                    if (commaCount >= 2) {
                        g_catchingHex = true;
                        // Serial.println("[4G] Ready to parse HEX data");
                    }
                }
            } else {
                // HEX 解析逻辑
                if (isHexadecimalDigit(ch)) {
                    if (!g_hasHighNibble) {
                        g_highNibble = hexCharToVal(ch);
                        g_hasHighNibble = true;
                    } else {
                        buf[totalReceived++] = (g_highNibble << 4) | hexCharToVal(ch);
                        g_hasHighNibble = false;
                        if (totalReceived >= wantLen) break;
                    }
                } else if (ch == '\r' || ch == '\n' || ch == ' ') {
                    // 遇到换行或空格，说明这一个小包解析完了，重置状态寻找下一个 +MIPRTCP
                    g_catchingHex = false;
                }
            }
        } else {
            vTaskDelay(1);
        }
    }
    return totalReceived;
}
// 补全丢失的 closeTCP 函数
void App4G::closeTCP() {
    _serial4G->println("AT+MIPCLOSE=1");
    // 调用 waitResponse 会自动处理并打印 OK
    waitResponse("OK", 1000);
    g_rxBuffer = ""; // 彻底清空防丢包缓冲区，为下次连接做准备
}
bool App4G::isConnected() { return _is_verified; }
String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; } 
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }
bool App4G::checkIP_manual() { return true; }