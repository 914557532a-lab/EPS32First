#ifndef APP_SERVER_H
#define APP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h> 
// 注意：虽然我们要用通用 Client，但 WiFi.h 还是得引用，因为 WiFiClient 定义在里面
// 如果未来完全不用 WiFi，可以换成 <Client.h>

class AppServer {
public:
    void init(const char* ip, uint16_t port);

    // [核心修改] 
    // 接收一个通用的 Client 指针 (可以是 WiFiClient* 也可以是 TinyGsmClient*)
    void chatWithServer(Client* networkClient);

private:
    const char* _server_ip;
    uint16_t _server_port;
    
    // 辅助函数也改为接收指针
    bool waitForData(Client* client, size_t len, uint32_t timeout_ms);
    bool readBigEndianInt(Client* client, uint32_t *val);
    void sendBigEndianInt(Client* client, uint32_t val);
};

extern AppServer MyServer;

#endif