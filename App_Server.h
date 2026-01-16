#ifndef APP_SERVER_H
#define APP_SERVER_H

#include <Arduino.h>
#include <Client.h>     // 引入基类 Client
#include <WiFiClient.h> // 引入 WiFiClient

class AppServer {
public:
    void init(const char* ip, uint16_t port);
    
    // [核心修改] 参数改为 Client* 以兼容 WiFiClient 和 TinyGsmClient
    void chatWithServer(Client* networkClient);

private:
    const char* _server_ip;
    uint16_t _server_port;

    // [核心修改] 辅助函数也全部改为 Client* 指针
    bool waitForData(Client* client, size_t len, uint32_t timeout_ms);
    bool readBigEndianInt(Client* client, uint32_t *val);
    void sendBigEndianInt(Client* client, uint32_t val);
};

extern AppServer MyServer;

#endif