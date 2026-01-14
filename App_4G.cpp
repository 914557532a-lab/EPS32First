/**
 * @file App_4G.cpp
 */
#include "App_4G.h"

App4G My4G;

void App4G::init() {
    // 1. 初始化电源引脚
    pinMode(PIN_4G_PWR, OUTPUT);
    digitalWrite(PIN_4G_PWR, LOW); // 先保持断电

    // 2. 初始化 PWRKEY (关键修改！)
    // 硬件接了 1K 下拉电阻(GND)，导致默认是 LOW (一直按着的状态)。
    // LE270-EU 需要我们主动拉高(HIGH)来"释放"按键，否则它会关机或不启动。
    pinMode(PIN_4G_PWRKEY, OUTPUT);
    digitalWrite(PIN_4G_PWRKEY, HIGH); // 强制拉高，对抗硬件下拉，模拟"松开按键"

    // 3. 处理背光冲突 (GPIO14)
    // 设为输入，把控制权交给 4G 模块的 NetLight
    pinMode(PIN_4G_NET, INPUT); 

    // 4. 初始化串口
    // 如果之后灯亮了但 AT 依然不通，请在这里互换 PIN_4G_RX 和 PIN_4G_TX
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);

    Serial.println("[4G] GPIO Init: PWRKEY set to HIGH (Released).");
}

void App4G::powerOn() {
    Serial.println("[4G] Starting LE270-EU Power Sequence...");

    // 步骤 1: 确保 PWRKEY 是释放状态 (High)
    digitalWrite(PIN_4G_PWRKEY, HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 步骤 2: 打开主电源 (GPIO45)
    Serial.println("[4G] Main Power ON (GPIO45 -> HIGH)");
    digitalWrite(PIN_4G_PWR, HIGH);
    
    // 等待电压稳定 (给足 1秒)
    vTaskDelay(pdMS_TO_TICKS(1000)); 

    // 步骤 3: 发送开机脉冲 (拉低 PWRKEY)
    Serial.println("[4G] Pressing PWRKEY (2s)...");
    digitalWrite(PIN_4G_PWRKEY, LOW);  // 拉低 (模拟按下)
    vTaskDelay(pdMS_TO_TICKS(2000));   // 保持 2秒 (LE270 通常需要 >1.5s)
    
    // 步骤 4: 释放 PWRKEY (恢复高电平)
    // 这一步至关重要！如果不拉高，模块会以为按键没松开，从而关机。
    Serial.println("[4G] Releasing PWRKEY...");
    digitalWrite(PIN_4G_PWRKEY, HIGH); // 拉高 (模拟松开)

    // 步骤 5: 等待模组启动
    Serial.println("[4G] Waiting for boot (8s)...");
    // 此时请观察 GPIO14 (屏幕背光) 是否开始闪烁
    for(int i=0; i<8; i++) {
        Serial.printf(" %d", i+1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.println();

    // 步骤 6: 初始化 TinyGSM
    if (_modem == nullptr) {
        _modem = new TinyGsm(*_serial4G);
    }
    if (_client == nullptr && _modem != nullptr) {
        _client = new TinyGsmClient(*_modem);
    }

    // 步骤 7: AT 握手测试
    Serial.println("[4G] Sending AT...");
    bool alive = false;
    for(int i=0; i<10; i++) {
        if (_modem->testAT(500)) { 
            alive = true;
            Serial.println("\n[4G] Module Response: OK! (Boot Success)");
            break;
        }
        Serial.print(".");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!alive) {
        Serial.println("\n[4G] No Response.");
        Serial.println("Checklist: 1. Did the LED flash? 2. Try swapping RX/TX.");
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