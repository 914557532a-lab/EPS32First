/**
 * @file App_4G.cpp
 */
#include "App_4G.h"

App4G My4G;

void App4G::init() {
    // 1. 初始化电源引脚
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); // 默认断电

    // 2. 初始化 PWRKEY - 【修改点】
    // 硬件接了 1K 下拉电阻(GND)，设计意图是"上电自启"。
    // 我们将其设为 INPUT (高阻态)，让硬件电阻发挥作用，保持为 LOW。
    // 不要输出 HIGH，否则会由于破坏了上电时的低电平条件而导致自启失败。
    pinMode(PIN_4G_PWRKEY, INPUT); 

    // 3. 处理背光冲突 (GPIO14)
    pinMode(PIN_4G_NET, INPUT); 

    // 4. 初始化串口
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);

    Serial.println("[4G] GPIO Init: PWRKEY set to INPUT (Using Hardware Auto-Start).");
}

void App4G::powerOn() {
    Serial.println("[4G] Starting LE270-EU Power Sequence (Auto-Start mode)...");

    // 步骤 1: 只要打开主电源，因为 PWRKEY 已经被硬件拉低，模块应自动开机
    Serial.println("[4G] Main Power ON (GPIO45 -> HIGH)");
    digitalWrite(PIN_4G_PWR, HIGH);
    
    // 步骤 2: 等待模组启动 (给予更长的缓冲时间)
    Serial.println("[4G] Waiting for boot (10s)...");
    for(int i=0; i<10; i++) {
        Serial.printf(" %d", i+1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.println();

    // 步骤 3: 初始化 TinyGSM
    if (_modem == nullptr) {
        _modem = new TinyGsm(*_serial4G);
    }
    if (_client == nullptr && _modem != nullptr) {
        _client = new TinyGsmClient(*_modem);
    }

    // 步骤 4: AT 握手测试
    Serial.println("[4G] Sending AT...");
    bool alive = false;
    // 尝试多次握手
    for(int i=0; i<20; i++) {
        if (_modem->testAT(500)) { 
            alive = true;
            Serial.println("\n[4G] Module Response: OK! (Boot Success)");
            break;
        }
        Serial.print(".");
        // 如果自动开机失败，这里可以作为补救措施：尝试手动拉低一下 PWRKEY
        // 但大概率不需要
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!alive) {
        Serial.println("\n[4G] No Response.");
        Serial.println("Try: Check if GPIO45 actually outputs High (Voltage).");
        
        // 补救措施，如果自动启动失败，尝试手动脉冲
        Serial.println("[4G] Auto-start failed. Trying manual pulse...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); // 先释放
        delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  // 按下
        delay(2500);                       // 长按2.5秒
        digitalWrite(PIN_4G_PWRKEY, HIGH); // 释放
        pinMode(PIN_4G_PWRKEY, INPUT);     // 恢复输入状态
    } else {
        String imei = getIMEI();
        Serial.println("[4G] IMEI: " + imei);
    }
}

// [修改] 支持超时设置，防止卡死
bool App4G::connect(unsigned long timeout_ms) {
    if (!_modem) return false;
    Serial.printf("[4G] Connecting... (Timeout: %lu)\n", timeout_ms);
    if (!_modem->waitForNetwork(timeout_ms)) return false;
    if (!_modem->gprsConnect(_apn.c_str(), _user.c_str(), _pass.c_str())) return false;
    Serial.println("[4G] Connected! IP: " + _modem->localIP().toString());
    return true;
}

bool App4G::isConnected() {
    return _modem && _modem->isNetworkConnected();
}

String App4G::getIMEI() {
    return _modem ? _modem->getIMEI() : "";
}

TinyGsmClient& App4G::getClient() {
    return *_client;
}

void App4G::sendRawAT(String cmd) {
    if (!_serial4G) return;
    Serial.println("CMD >> " + cmd);
    _serial4G->println(cmd);
    unsigned long start = millis();
    while (millis() - start < 2000) {
        if (_serial4G->available()) {
            while (_serial4G->available()) Serial.write(_serial4G->read());
            start = millis(); 
        }
        delay(10);
    }
    Serial.println();
}