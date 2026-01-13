/**
 * @file App_Server.cpp
 * @brief Socket 客户端实现 - 支持 WiFi/4G 通用接口
 */
#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 

AppServer MyServer;

void AppServer::init(const char* ip, uint16_t port) {
    _server_ip = ip;
    _server_port = port;
    Serial.printf("[Server] Configured: %s:%d\n", _server_ip, _server_port);
}

// [修改] 接收 Client* 指针，使用 -> 操作符
bool AppServer::waitForData(Client* client, size_t len, uint32_t timeout_ms) {
    unsigned long start = millis();
    while (client->available() < len) {
        if (millis() - start > timeout_ms) {
            return false; // 超时
        }
        // 关键：给 IDLE 任务运行机会，重置看门狗
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    return true;
}

// [修改] 接收 Client* 指针
bool AppServer::readBigEndianInt(Client* client, uint32_t *val) {
    // 先等待 4 字节数据，超时设为 5000ms
    if (!waitForData(client, 4, 5000)) return false;

    uint8_t buf[4];
    int readLen = client->readBytes(buf, 4);
    if (readLen != 4) return false;
    
    *val = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return true;
}

// [修改] 接收 Client* 指针
void AppServer::sendBigEndianInt(Client* client, uint32_t val) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
    client->write(buf, 4);
}

// [核心修改] 逻辑入口：接收外部传入的网络客户端 (WiFiClient 或 TinyGsmClient)
void AppServer::chatWithServer(Client* networkClient) {
    if (networkClient == NULL) {
         Serial.println("[Server] Error: No Network Client!");
         MyUILogic.updateAssistantStatus("网络错误");
         MyUILogic.finishAIState();
         return;
    }

    Serial.printf("[Server] Connecting to %s:%d...\n", _server_ip, _server_port);
    MyUILogic.updateAssistantStatus("连接中...");

    // 使用传入的 client 指针进行连接
    if (!networkClient->connect(_server_ip, _server_port)) {
        Serial.println("[Server] Connect Failed.");
        MyUILogic.updateAssistantStatus("连接失败");
        return;
    }

    // --- 1. 发送录音数据 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;

    if (audioSize == 0 || audioData == NULL) {
        Serial.println("[Server] No record data!");
        networkClient->stop();
        return;
    }

    Serial.printf("[Server] Sending Audio: %d bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    sendBigEndianInt(networkClient, audioSize);
    
    const int chunkSize = 1024;
    uint32_t sent = 0;
    while (sent < audioSize) {
        int toSend = (audioSize - sent) > chunkSize ? chunkSize : (audioSize - sent);
        networkClient->write(audioData + sent, toSend);
        sent += toSend;
        
        // 稍微让出 CPU，防止触发看门狗
        if (sent % 10240 == 0) vTaskDelay(1); 
    }
    networkClient->flush();
    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 2. 接收响应 ---
    
    // A. 等待并读取 JSON 长度
    uint32_t jsonLen = 0;
    if (!readBigEndianInt(networkClient, &jsonLen)) { 
        Serial.println("[Server] Timeout: No Reply from AI");
        MyUILogic.updateAssistantStatus("无应答");
        networkClient->stop();
        MyUILogic.finishAIState();
        return;
    }
    Serial.printf("[Server] JSON Length: %d\n", jsonLen);

    // B. 读取 JSON 内容
    if (jsonLen > 0) {
        if (!waitForData(networkClient, jsonLen, 5000)) { // 给稍微长一点的超时
            Serial.println("[Server] Err: JSON data incomplete");
            networkClient->stop();
            MyUILogic.finishAIState();
            return;
        }

        char *jsonBuf = (char*)malloc(jsonLen + 1);
        if (jsonBuf) {
            networkClient->readBytes(jsonBuf, jsonLen);
            jsonBuf[jsonLen] = 0; 
            Serial.printf("[Server] JSON: %s\n", jsonBuf);
            
            // UI 更新与指令处理
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, jsonBuf);
            if (!error) {
                const char* reply = doc["reply_text"];
                if (reply) MyUILogic.showReplyText(reply);
                
                MyUILogic.handleAICommand(String(jsonBuf)); 
            }
            free(jsonBuf);
        }
    }

    // C. 读取 音频 长度
    uint32_t pcmLen = 0;
    if (!readBigEndianInt(networkClient, &pcmLen)) {
        Serial.println("[Server] Err: Failed to read PCM len");
        networkClient->stop();
        MyUILogic.finishAIState();
        return;
    }
    Serial.printf("[Server] PCM Length: %d\n", pcmLen);

    // D. 播放音频
    if (pcmLen > 0) {
        MyUILogic.updateAssistantStatus("回复中...");
        // 这里的 playStream 需要修改 App_Audio.h/.cpp 里的定义吗？
        // 通常 Stream* 是 Client* 的父类，所以直接传 networkClient 应该没问题。
        // 但如果你的 App_Audio::playStream 定义的是 WiFiClient*，那也得改！
        // 假设 App_Audio 接收的是 Stream* 或者 Client*
        MyAudio.playStream(networkClient, pcmLen);
    }

    Serial.println("[Server] Transaction Complete.");
    networkClient->stop();
    MyUILogic.finishAIState();
}