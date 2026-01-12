/**
 * @file App_Server.cpp
 * @brief Socket 客户端实现 - 修复看门狗复位问题
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

// [修复 2] 非阻塞等待数据函数
// 等待指定长度的数据，期间喂狗(yield)
bool AppServer::waitForData(size_t len, uint32_t timeout_ms) {
    unsigned long start = millis();
    while (client.available() < len) {
        if (millis() - start > timeout_ms) {
            return false; // 超时
        }
        // 关键：给 IDLE 任务运行机会，重置看门狗
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    return true;
}

// 辅助：读取4字节大端整数
bool AppServer::readBigEndianInt(uint32_t *val) {
    // 先等待 4 字节数据，超时设为 5000ms
    if (!waitForData(4, 5000)) return false;

    uint8_t buf[4];
    int readLen = client.readBytes(buf, 4);
    if (readLen != 4) return false;
    
    *val = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    return true;
}

// 辅助：发送4字节大端整数
void AppServer::sendBigEndianInt(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
    client.write(buf, 4);
}

void AppServer::chatWithServer() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Server] No WiFi!");
        MyUILogic.updateAssistantStatus("网络断开");
        return;
    }

    Serial.printf("[Server] Connecting to %s:%d...\n", _server_ip, _server_port);
    MyUILogic.updateAssistantStatus("连接中...");

    if (!client.connect(_server_ip, _server_port)) {
        Serial.println("[Server] Connect Failed.");
        MyUILogic.updateAssistantStatus("连接失败");
        return;
    }

    // --- 1. 发送录音数据 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;

    if (audioSize == 0 || audioData == NULL) {
        Serial.println("[Server] No record data!");
        client.stop();
        return;
    }

    Serial.printf("[Server] Sending Audio: %d bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    sendBigEndianInt(audioSize);
    
    const int chunkSize = 1024;
    uint32_t sent = 0;
    while (sent < audioSize) {
        int toSend = (audioSize - sent) > chunkSize ? chunkSize : (audioSize - sent);
        client.write(audioData + sent, toSend);
        sent += toSend;
        // 发送大文件时也稍微让一下 CPU，防止触发 TX 看门狗（虽然较少见）
        if (sent % 10240 == 0) vTaskDelay(1); 
    }
    client.flush();
    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 2. 接收响应 ---
    
    // A. 等待并读取 JSON 长度 (给予 AI 最多 15 秒思考时间)
    // 注意：这里使用 waitForData 代替 setTimeout，避免阻塞死锁
    uint32_t jsonLen = 0;
    if (!readBigEndianInt(&jsonLen)) { // 内部已包含 wait
        Serial.println("[Server] Timeout: No Reply from AI");
        MyUILogic.updateAssistantStatus("无应答");
        client.stop();
        MyUILogic.finishAIState();
        return;
    }
    Serial.printf("[Server] JSON Length: %d\n", jsonLen);

    // B. 读取 JSON 内容
    if (jsonLen > 0) {
        // 等待数据到齐
        if (!waitForData(jsonLen, 3000)) {
            Serial.println("[Server] Err: JSON data incomplete");
            client.stop();
            MyUILogic.finishAIState();
            return;
        }

        char *jsonBuf = (char*)malloc(jsonLen + 1);
        if (jsonBuf) {
            client.readBytes(jsonBuf, jsonLen);
            jsonBuf[jsonLen] = 0; 
            Serial.printf("[Server] JSON: %s\n", jsonBuf);
            
            // UI 更新
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, jsonBuf);
            if (!error) {
                const char* reply = doc["reply_text"];
                if (reply) MyUILogic.showReplyText(reply);
                
                // 处理指令
                MyUILogic.handleAICommand(String(jsonBuf)); // 传入完整 JSON 字符串给 Logic 处理
            }
            free(jsonBuf);
        }
    }

    // C. 读取 音频 长度
    uint32_t pcmLen = 0;
    if (!readBigEndianInt(&pcmLen)) {
        Serial.println("[Server] Err: Failed to read PCM len");
        client.stop();
        MyUILogic.finishAIState();
        return;
    }
    Serial.printf("[Server] PCM Length: %d\n", pcmLen);

    // D. 播放音频
    if (pcmLen > 0) {
        MyUILogic.updateAssistantStatus("回复中...");
        // playStream 内部有 while client.connected() 循环
        // 请确保 playStream 里也有 yield 或 vTaskDelay (之前的代码里已经有了)
        MyAudio.playStream(&client, pcmLen);
    }

    Serial.println("[Server] Transaction Complete.");
    client.stop();
    MyUILogic.finishAIState();
}