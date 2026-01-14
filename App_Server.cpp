#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     

AppServer MyServer;

void AppServer::init(const char* ip, uint16_t port) {
    _server_ip = ip;
    _server_port = port;
    Serial.printf("[Server] Configured: %s:%d\n", _server_ip, _server_port);
}

// 辅助函数：手动发送长度包
bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
    return My4G.sendData(buf, 4);
}

bool AppServer::waitForData(Client* client, size_t len, uint32_t timeout_ms) {
    unsigned long start = millis();
    while (client->available() < len) {
        if (millis() - start > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    return true;
}

bool AppServer::readBigEndianInt(Client* client, uint32_t *val) {
    if (!waitForData(client, 4, 5000)) return false;
    uint8_t buf[4];
    if (client->readBytes(buf, 4) != 4) return false;
    *val = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return true;
}

void AppServer::sendBigEndianInt(Client* client, uint32_t val) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
    client->write(buf, 4);
}

// --- 核心逻辑 ---
void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    
    Serial.printf("[Server] Connecting to %s:%d (Mode: %s)...\n", 
                  _server_ip, _server_port, isWiFi ? "WiFi" : "4G");
    
    MyUILogic.updateAssistantStatus("连接中...");

    bool connected = false;
    if (isWiFi) {
        if (networkClient && networkClient->connect(_server_ip, _server_port)) {
            connected = true;
        }
    } else {
        // 4G 模式：调用手动连接函数
        if (My4G.connectTCP(_server_ip, _server_port)) {
            connected = true;
        }
    }

    if (!connected) {
        Serial.println("[Server] Connect Failed.");
        MyUILogic.updateAssistantStatus("连接失败");
        return;
    }

    // --- 发送录音数据 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;
    Serial.printf("[Server] Sending Audio: %d bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) {
        sendBigEndianInt(networkClient, audioSize);
        networkClient->write(audioData, audioSize);
        networkClient->flush();
    } else {
        // [4G 稳健发送]
        delay(500); // 稍微等一下连接稳定

        // 1. 发送长度包
        if (!sendIntManual(audioSize)) {
            Serial.println("[Server] 4G Send Length Failed! Aborting.");
            MyUILogic.updateAssistantStatus("发送失败");
            My4G.closeTCP();
            return;
        }

        // 2. 发送音频数据
        size_t sent = 0;
        size_t chunkSize = 512; 
        bool sendOk = true;
        
        while (sent < audioSize) {
            size_t left = audioSize - sent;
            size_t toSend = (left > chunkSize) ? chunkSize : left;
            if (!My4G.sendData(audioData + sent, toSend)) {
                Serial.println("[Server] 4G Send Body Fail");
                sendOk = false;
                break;
            }
            sent += toSend;
            delay(50); 
        }
        
        if (!sendOk) {
             My4G.closeTCP();
             return;
        }
    }

    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 接收逻辑 ---
    uint32_t jsonLen = 0;
    
    if (isWiFi) {
        // WiFi 接收
        if (readBigEndianInt(networkClient, &jsonLen)) { 
            Serial.printf("[Server] Reply Len: %d\n", jsonLen);
            if (jsonLen > 0) {
                 char *jsonBuf = (char*)malloc(jsonLen + 1);
                 if (jsonBuf) {
                     waitForData(networkClient, jsonLen, 5000);
                     networkClient->readBytes(jsonBuf, jsonLen);
                     jsonBuf[jsonLen] = 0;
                     Serial.printf("[Server] JSON: %s\n", jsonBuf);
                     MyUILogic.handleAICommand(String(jsonBuf));
                     free(jsonBuf);
                 }
            }
        }
    } else {
        // 4G 接收 (使用 readData 解析推送)
        uint8_t lenBuf[4];
        if (My4G.readData(lenBuf, 4, 20000) == 4) {
            jsonLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
            Serial.printf("[Server] 4G Reply JSON Len: %d\n", jsonLen);
            
            if (jsonLen > 0 && jsonLen < 4096) {
                char *jsonBuf = (char*)malloc(jsonLen + 1);
                if (jsonBuf) {
                    if (My4G.readData((uint8_t*)jsonBuf, jsonLen, 5000) == jsonLen) {
                        jsonBuf[jsonLen] = 0;
                        Serial.printf("[Server] JSON: %s\n", jsonBuf);
                        MyUILogic.handleAICommand(String(jsonBuf));
                    }
                    free(jsonBuf);
                }
            }
            MyUILogic.updateAssistantStatus("收到回复");
        } else {
            Serial.println("[Server] 4G Wait Reply Timeout");
        }
    }

    if (isWiFi) networkClient->stop(); else My4G.closeTCP();
    MyUILogic.finishAIState();
}