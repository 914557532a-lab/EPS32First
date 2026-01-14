#include "App_4G.h"

App4G My4G;

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
    
    Serial.println("[4G] Waiting for boot (10s)...");
    for(int i=0; i<10; i++) {
        Serial.printf(" %d", i+1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    Serial.println();

    if (_modem == nullptr) {
        _modem = new TinyGsm(*_serial4G);
    }
    if (_client == nullptr && _modem != nullptr) {
        _client = new TinyGsmClient(*_modem);
    }

    Serial.println("[4G] Sending AT...");
    bool alive = false;
    for(int i=0; i<20; i++) {
        if (_modem->testAT(500)) { 
            alive = true;
            Serial.println("\n[4G] Module Alive!");
            break;
        }
        Serial.print(".");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!alive) {
        Serial.println("\n[4G] No Response. Trying pulse...");
        pinMode(PIN_4G_PWRKEY, OUTPUT);
        digitalWrite(PIN_4G_PWRKEY, HIGH); delay(100);
        digitalWrite(PIN_4G_PWRKEY, LOW);  delay(2500);
        digitalWrite(PIN_4G_PWRKEY, HIGH); 
        pinMode(PIN_4G_PWRKEY, INPUT);   
    }
}

void App4G::sendRawAT(String cmd) {
    if (!_serial4G) return;
    while(_serial4G->available()) _serial4G->read(); 
    _serial4G->println(cmd);
    delay(200); 
    while (_serial4G->available()) Serial.write(_serial4G->read());
    Serial.println();
}

bool App4G::checkIP_manual() {
    if (!_serial4G) return false;
    while(_serial4G->available()) _serial4G->read();
    
    _serial4G->println("AT+CGDCONT?");
    String response = "";
    unsigned long start = millis();
    while (millis() - start < 1000) { // 缩短等待时间，避免日志刷屏太慢
        while (_serial4G->available()) response += (char)_serial4G->read();
        delay(10);
    }
    
    if (response.indexOf("\"0.0.0.0\"") == -1 && 
        (response.indexOf("\"10.") != -1 || response.indexOf("\"172.") != -1 || 
         response.indexOf("\"100.") != -1 || response.indexOf("\"192.") != -1)) {
        // Serial.println("[4G] IP Found."); // 减少刷屏，注释掉
        return true;
    }
    return false;
}

bool App4G::ensureNetOpen() {
    Serial.println("[4G] Checking NETOPEN status...");
    
    // 1. 先查询
    _serial4G->println("AT+NETOPEN?");
    String resp = "";
    unsigned long start = millis();
    while (millis() - start < 1000) {
        while (_serial4G->available()) resp += (char)_serial4G->read();
        delay(10);
    }

    if (resp.indexOf(": 1") != -1 || resp.indexOf("opened") != -1) {
        Serial.println("[4G] Network Service is OPEN.");
        return true;
    }

    // 2. 尝试打开
    Serial.println("[4G] Trying to OPEN Network Service...");
    sendRawAT("AT+NETOPEN"); 
    
    // 3. 再次查询 (如果打开失败，可能是已经开了但没查到，或者真挂了)
    delay(1000);
    _serial4G->println("AT+NETOPEN?");
    resp = "";
    start = millis();
    while (millis() - start < 1000) {
        while (_serial4G->available()) resp += (char)_serial4G->read();
        delay(10);
    }
    
    if (resp.indexOf(": 1") != -1) {
        Serial.println("[4G] Network Service OPEN SUCCESS!");
        return true;
    }
    
    // [宽容处理] 如果前面查到了 IP，就算 NETOPEN 报错，我们也返回 true 试试
    // 因为有些固件版本 NETOPEN 行为不一致
    Serial.println("[4G] Warning: NETOPEN check failed, but proceeding anyway.");
    return true; 
}

void App4G::checkDNS() {
    Serial.println("[4G] Testing DNS (www.baidu.com)...");
    sendRawAT("AT+CDNSGIP=\"www.baidu.com\"");
}

bool App4G::connect(unsigned long timeout_ms) {
    if (!_modem) return false;
    
    // 1. 检查 IP (这是最关键的)
    if (checkIP_manual()) {
        Serial.println("[4G] IP detected. Verifying Services...");
        
        // 2. 确保 NETOPEN (Socket服务)
        ensureNetOpen(); // 不管返回啥，都继续
        
        // 3. 检查 DNS (仅做调试，不再因为失败而返回 false)
        checkDNS();
        
        _is_verified = true; 
        Serial.println("[4G] >>> READY FOR TCP <<<");
        return true;
    }

    // ... 下面的常规拨号逻辑保持不变 ...
    if (timeout_ms < 30000) timeout_ms = 30000;
    if (!_modem->waitForNetwork(timeout_ms)) return false;
    sendRawAT("AT+CGDCONT=1,\"IP\",\"" + _apn + "\""); 
    if (!_modem->gprsConnect(_apn.c_str(), _user.c_str(), _pass.c_str())) {
        if (checkIP_manual()) {
             ensureNetOpen();
             _is_verified = true;
             return true;
        }
        return false;
    }
    ensureNetOpen();
    _is_verified = true;
    return true;
}

// [修改] 只有当 IP 存在 且 经过了 connect() 验证后，才返回 true
bool App4G::isConnected() {
    if (!_modem) return false;
    return checkIP_manual() && _is_verified;
}

String App4G::getIMEI() { return _modem ? _modem->getIMEI() : ""; }
TinyGsmClient& App4G::getClient() { return *_client; }
int App4G::getSignalCSQ() { return _modem ? _modem->getSignalQuality() : 0; }