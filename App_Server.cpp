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
        if (My4G.connectTCP(_server_ip, _server_port)) {
            connected = true;
        }
    }

    if (!connected) {
        Serial.println("[Server] Connect Failed.");
        MyUILogic.updateAssistantStatus("连接失败");
        return;
    }

    // --- 发送逻辑 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;
    Serial.printf("[Server] Sending Audio: %d bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) {
        sendBigEndianInt(networkClient, audioSize);
        networkClient->write(audioData, audioSize);
        networkClient->flush();
    } else {
        // 4G 发送
        delay(500); 
        if (!sendIntManual(audioSize)) {
            Serial.println("[Server] 4G Send Length Failed!");
            MyUILogic.updateAssistantStatus("发送失败");
            My4G.closeTCP();
            return;
        }
        size_t sent = 0;
        size_t chunkSize = 512; 
        bool sendOk = true;
        while (sent < audioSize) {
            size_t left = audioSize - sent;
            size_t toSend = (left > chunkSize) ? chunkSize : left;
            if (!My4G.sendData(audioData + sent, toSend)) {
                sendOk = false; break;
            }
            sent += toSend;
            delay(50); 
        }
        if (!sendOk) { My4G.closeTCP(); return; }
    }

    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 接收逻辑 ---
    uint32_t jsonLen = 0;
    uint32_t audioLen = 0;
    
    if (isWiFi) {
        // WiFi 逻辑保持不变
        if (readBigEndianInt(networkClient, &jsonLen)) { 
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
            if (readBigEndianInt(networkClient, &audioLen)) {
                MyUILogic.updateAssistantStatus("正在回复");
                if (audioLen > 0 && audioLen < AUDIO_BUFFER_SIZE) {
                     waitForData(networkClient, audioLen, 10000);
                     networkClient->readBytes(MyAudio.record_buffer, audioLen);
                     MyAudio.playChunk(MyAudio.record_buffer, audioLen);
                }
            }
        }
    } else {
        // === 4G 接收逻辑 (先下载到内存，再播放) ===
        uint8_t lenBuf[4];
        
        Serial.println("[Server] 4G Waiting for JSON Header...");
        if (My4G.readData(lenBuf, 4, 60000) == 4) { // 60秒等待
            
            // 1. JSON
            jsonLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
            Serial.printf("[Server] 4G JSON Len: %d\n", jsonLen);
            
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
            
            // 2. 音频长度
            Serial.println("[Server] 4G Waiting for Audio Header...");
            if (My4G.readData(lenBuf, 4, 10000) == 4) {
                audioLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
                Serial.printf("[Server] 4G Audio Len: %d\n", audioLen);
                
                MyUILogic.updateAssistantStatus("接收语音...");
                
                // 【关键修改】不要边收边播！先全部收下来！
                // 我们直接复用 MyAudio.record_buffer 这个大内存
                // 确保不要超过缓冲区大小
                if (audioLen > AUDIO_BUFFER_SIZE) audioLen = AUDIO_BUFFER_SIZE;

                size_t totalReceived = 0;
                size_t chunk = 1024; // 每次读 1KB
                bool rxSuccess = true;

                while (totalReceived < audioLen) {
                    size_t left = audioLen - totalReceived;
                    size_t toRead = (left > chunk) ? chunk : left;
                    
                    // 只要还有数据，就拼命读，不阻塞去播放
                    size_t actual = My4G.readData(MyAudio.record_buffer + totalReceived, toRead, 10000);
                    
                    if (actual > 0) {
                        totalReceived += actual;
                        // 打印个点，看进度
                        if (totalReceived % 10240 == 0) Serial.print("."); 
                    } else {
                        Serial.println("\n[Server] 4G Read Audio Timeout/Error");
                        rxSuccess = false;
                        break;
                    }
                }
                Serial.println("\n[Server] 4G Download Complete.");
                
                // 3. 收完之后，一次性播放
                if (rxSuccess) {
                    MyUILogic.updateAssistantStatus("正在回复");
                    Serial.println("[Server] Playing Audio from RAM...");
                    MyAudio.playChunk(MyAudio.record_buffer, totalReceived);
                }
                
            } else {
                Serial.println("[Server] 4G Audio Length Header Missing");
            }
        } else {
            Serial.println("[Server] 4G Wait JSON Reply Timeout");
        }
    }

    if (isWiFi) networkClient->stop(); else My4G.closeTCP();
    MyUILogic.finishAIState();
}