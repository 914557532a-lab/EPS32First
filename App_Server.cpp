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

// [App_Server.cpp] 完整修复版：基于仓库模式的 chatWithServer
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
        // 4G 模式：调用 My4G 维护的 TCP 连接
        if (My4G.connectTCP(_server_ip, _server_port)) {
            connected = true;
        }
    }

    if (!connected) {
        Serial.println("[Server] Connect Failed.");
        MyUILogic.updateAssistantStatus("连接失败");
        return;
    }

    // --- 1. 发送录音数据 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;
    Serial.printf("[Server] Sending Audio: %u bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) {
        sendBigEndianInt(networkClient, audioSize);
        networkClient->write(audioData, audioSize);
        networkClient->flush();
    } else {
        // 4G 发送：先发长度包头，再分块发送原始数据
        delay(500); 
        if (!sendIntManual(audioSize)) {
            Serial.println("[Server] 4G Send Length Failed!");
            MyUILogic.updateAssistantStatus("发送失败");
            My4G.closeTCP();
            return;
        }
        size_t sent = 0;
        size_t chunkSize = 1024; 
        while (sent < audioSize) {
            size_t left = audioSize - sent;
            size_t toSend = (left > chunkSize) ? chunkSize : left;
            if (!My4G.sendData(audioData + sent, toSend)) break;
            sent += toSend;
            vTaskDelay(pdMS_TO_TICKS(10)); 
        }
    }

    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 2. 接收响应数据 ---
    uint32_t jsonLen = 0;
    uint32_t audioLen = 0;
    
    if (isWiFi) {
        // WiFi 接收逻辑 (原生 Stream)
        if (readBigEndianInt(networkClient, &jsonLen)) { 
            if (jsonLen > 0 && jsonLen < 4096) {
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
            if (readBigEndianInt(networkClient, &audioLen)) {
                if (audioLen > 0 && audioLen < AUDIO_BUFFER_SIZE) {
                     MyUILogic.updateAssistantStatus("正在回复");
                     waitForData(networkClient, audioLen, 15000);
                     networkClient->readBytes(MyAudio.record_buffer, audioLen);
                     MyAudio.playChunk(MyAudio.record_buffer, audioLen);
                }
            }
        }
    } else {
        // === 4G 接收逻辑 (仓库模式： readData 已自动完成 HEX 转换) ===
        uint8_t lenBuf[4];
        
        // (A) 读取 JSON 长度头
        Serial.println("[Server] 4G Waiting for JSON Header...");
        int rxHeader = My4G.readData(lenBuf, 4, 30000); 
        
        if (rxHeader == 4) { 
            jsonLen = (uint32_t)((lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3]);
            Serial.printf("[Server] 4G JSON Len: %u\n", jsonLen);
            
            if (jsonLen > 0 && jsonLen < 4096) {
                char *jsonBuf = (char*)malloc(jsonLen + 1);
                if (jsonBuf) {
                    // 读取 JSON 正文
                    if (My4G.readData((uint8_t*)jsonBuf, jsonLen, 10000) == jsonLen) {
                        jsonBuf[jsonLen] = 0;
                        Serial.printf("[Server] 4G JSON Content: %s\n", jsonBuf);
                        MyUILogic.handleAICommand(String(jsonBuf)); 
                    }
                    free(jsonBuf);
                }
            }

            // (B) 读取音频长度头
            Serial.println("[Server] 4G Waiting for Audio Header...");
            if (My4G.readData(lenBuf, 4, 15000) == 4) {
                audioLen = (uint32_t)((lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3]);
                
                if (audioLen > 0 && audioLen < AUDIO_BUFFER_SIZE) {
                    Serial.printf("[Server] 4G Audio Len: %u bytes\n", audioLen);
                    MyUILogic.updateAssistantStatus("正在回复");
                    
                    // 直接从仓库大批量提取音频二进制数据
                    size_t actualRx = My4G.readData(MyAudio.record_buffer, audioLen, 30000);
                    if (actualRx > 0) {
                        Serial.printf("[Server] 4G Rx Success: %u bytes. Playing...\n", actualRx);
                        MyAudio.playChunk(MyAudio.record_buffer, actualRx);
                    }
                }
            } else {
                Serial.println("[Server] 4G Audio Header Timeout");
            }
        } else {
            Serial.printf("[Server] 4G JSON Header Timeout (Recv: %d bytes)\n", rxHeader);
        }
    }

    // --- 3. 资源清理 ---
    if (isWiFi) {
        networkClient->stop(); 
    } else {
        My4G.closeTCP();
    }
    
    MyUILogic.finishAIState();
    Serial.println("[Server] Transaction Complete.");
}