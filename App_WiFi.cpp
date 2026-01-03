#include "App_WiFi.h"

AppWiFi MyWiFi;

void AppWiFi::init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true); // 掉线会自动重连，很省心
    
    // 功率设置保持你原来的逻辑
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    
    // 关闭节能模式，保证 UI 和网络响应速度
    WiFi.setSleep(false); 
    
    Serial.println("[WiFi] Initialized.");
    Serial.print("[WiFi] Device MAC: ");
    Serial.println(WiFi.macAddress());
}

void AppWiFi::connect(const char* ssid, const char* password) {
    // 状态检查：防止重复连接
    if (WiFi.status() == WL_CONNECTED) {
        if (String(WiFi.SSID()) == String(ssid)) {
            Serial.println("[WiFi] Already connected to this SSID.");
            return;
        }
    }

    Serial.printf("[WiFi] Connecting to [%s] ...\n", ssid);
    
    // 这里的 disconnect 和 begin 都是非阻塞的，瞬间执行完
    WiFi.disconnect(true); 
    WiFi.begin(ssid, password);
}

bool AppWiFi::isConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

wl_status_t AppWiFi::getStatus() {
    return WiFi.status();
}

String AppWiFi::getIP() {
    if (isConnected()) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

String AppWiFi::getMac() {
    return WiFi.macAddress();
}

int AppWiFi::getRSSI() {
    if (isConnected()) {
        return WiFi.RSSI();
    }
    return -100; 
}

// 仅仅打印日志，没有任何延时或循环
void AppWiFi::logStatus() {
    if (isConnected()) {
        // 只有变化时或者调试时才打印，避免刷屏
        // Serial.printf("[WiFi] IP: %s, Signal: %d dBm\n", getIP().c_str(), getRSSI());
    } else {
        Serial.printf("[WiFi] Status: %d\n", WiFi.status());
    }
}