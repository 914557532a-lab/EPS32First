#include "App_4G.h"

App4G My4G;

void App4G::init() {
    // 1. 初始化控制引脚
    pinMode(PIN_4G_PWR_EN, OUTPUT);  
    pinMode(PIN_4G_PWR_KEY, OUTPUT); 
    
    // 默认引脚状态 (断电)
    digitalWrite(PIN_4G_PWR_EN, LOW);   
    digitalWrite(PIN_4G_PWR_KEY, HIGH); 

    // 2. 初始化串口
    // 请确保 Pin_Config.h 中定义了 PIN_4G_RX 和 PIN_4G_TX
    _serial4G->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);

    Serial.println("[4G] Hardware GPIO Initialized (Power OFF).");
}

void App4G::powerOn() {
    Serial.println("[4G] Powering ON (Task Context)...");

    // 步骤 1: 打开主电源
    digitalWrite(PIN_4G_PWR_EN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500)); // 让出 CPU 0.5秒

    // 步骤 2: 拉低 PWRKEY 开机
    Serial.println("[4G] Toggling PWRKEY...");
    digitalWrite(PIN_4G_PWR_KEY, LOW);  
    vTaskDelay(pdMS_TO_TICKS(2000)); // 拉低 2秒
    digitalWrite(PIN_4G_PWR_KEY, HIGH); 

    // 步骤 3: 等待模组启动
    Serial.println("[4G] Waiting for module boot...");
    vTaskDelay(pdMS_TO_TICKS(3000)); 

    // 步骤 4: 初始化 TinyGSM 对象
    // 使用 new 动态分配，防止未初始化时占用内存或静态构造顺序问题
    if (_modem == nullptr) {
        _modem = new TinyGsm(*_serial4G);
    }
    if (_client == nullptr && _modem != nullptr) {
        _client = new TinyGsmClient(*_modem);
    }

    // 步骤 5: 握手测试 (检测 AT 通道是否通畅)
    bool alive = false;
    for(int i=0; i<10; i++) {
        Serial.print(".");
        if (_modem->testAT()) {
            alive = true;
            Serial.println("\n[4G] Module is Alive!");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!alive) {
        Serial.println("\n[4G] Error: No response from module.");
    } else {
        // 获取一下 IMEI 确认身份
        String imei = getIMEI();
        Serial.println("[4G] IMEI: " + imei);
    }
}

bool App4G::connect() {
    if (!_modem) {
        Serial.println("[4G] Error: Modem object not initialized. Call powerOn() first.");
        return false;
    }

    Serial.print("[4G] Waiting for network registration...");
    // 等待网络注册 (自动处理 AT+CEREG 等)
    if (!_modem->waitForNetwork()) {
        Serial.println(" Failed!");
        return false;
    }
    Serial.println(" Registered!");

    Serial.print("[4G] Connecting to APN: ");
    Serial.print(_apn);
    
    // 建立 GPRS/PDP 连接 (自动处理 AT+MIPCALL / AT+CGACT 等)
    if (!_modem->gprsConnect(_apn.c_str(), _user.c_str(), _pass.c_str())) {
        Serial.println(" ... Connect Failed!");
        return false;
    }

    Serial.println(" ... Connected!");
    Serial.print("[4G] IP Address: ");
    Serial.println(_modem->localIP());
    
    return true;
}

bool App4G::isConnected() {
    if (!_modem) return false;
    return _modem->isNetworkConnected();
}

String App4G::getIMEI() {
    if (!_modem) return "";
    return _modem->getIMEI();
}

TinyGsmClient& App4G::getClient() {
    // 如果 _client 指针为空，说明还没 powerOn，这里做一个简单的防崩处理
    // 实际调用前请务必确保 powerOn 已执行
    if (!_client) {
        Serial.println("[4G] CRITICAL: Client accessed before init!");
    }
    return *_client;
}