#ifndef APP_SERVER_H
#define APP_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h> 

class AppServer {
public:
    void init(const char* ip, uint16_t port);
    void chatWithServer();

private:
    const char* _server_ip;
    uint16_t _server_port;
    WiFiClient client;

    // [新增] 带有超时和喂狗功能的等待函数
    bool waitForData(size_t len, uint32_t timeout_ms);

    bool readBigEndianInt(uint32_t *val);
    void sendBigEndianInt(uint32_t val);
};

extern AppServer MyServer;

#endif // APP_SERVER_H