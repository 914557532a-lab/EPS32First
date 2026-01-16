#ifndef APP_SERVER_H
#define APP_SERVER_H

#include <Arduino.h>
#include <Client.h>     // 引入基类 Client
#include <WiFiClient.h> // 引入 WiFiClient

class AppServer {
public:
    void init(const char* ip, uint16_t port);
    
    // 核心交互逻辑 (支持 WiFiClient 和 TinyGsmClient)
    void chatWithServer(Client* networkClient);

private:
    const char* _server_ip;
    uint16_t _server_port;
};

extern AppServer MyServer;

#endif