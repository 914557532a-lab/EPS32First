#ifndef APP_SERVER_H
#define APP_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h> // 需要安装 ArduinoJson 库

class AppServer {
public:
    void init(const char* ip, int port);
    
    // 上传录音并等待回复 (阻塞执行)
    void chatWithServer();

private:
    const char* server_ip;
    int server_port;
};

extern AppServer MyServer;

#endif