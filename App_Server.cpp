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

// --- [App_Server.cpp] 完整修复版 chatWithServer 函数 ---
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

    // --- 1. 发送逻辑 ---
    uint32_t audioSize = MyAudio.record_data_len;
    uint8_t *audioData = MyAudio.record_buffer;
    Serial.printf("[Server] Sending Audio: %d bytes\n", audioSize);
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) {
        sendBigEndianInt(networkClient, audioSize);
        networkClient->write(audioData, audioSize);
        networkClient->flush();
    } else {
        // 4G 发送逻辑
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
            delay(50); // 发送包间隔
        }
        if (!sendOk) { My4G.closeTCP(); return; }
    }

    Serial.println("[Server] Sent Done. Waiting for reply...");
    MyUILogic.updateAssistantStatus("思考中...");

    // --- 2. 接收逻辑 ---
    uint32_t jsonLen = 0;
    uint32_t audioLen = 0;
    
    if (isWiFi) {
        // WiFi 逻辑 (保持原生 Stream 方式)
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
        // === 4G 接收逻辑 (强化安全版：防止看门狗重启) ===
        uint8_t lenBuf[4];
        
        Serial.println("[Server] 4G Waiting for JSON Header...");
        // 1. 尝试读取 JSON 长度包头
        if (My4G.readData(lenBuf, 4, 30000) == 4) { 
            jsonLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
            Serial.printf("[Server] 4G JSON Len: %d\n", jsonLen);
            
            if (jsonLen > 0 && jsonLen < 4096) {
                char *jsonBuf = (char*)malloc(jsonLen + 1);
                if (jsonBuf) {
                    if (My4G.readData((uint8_t*)jsonBuf, jsonLen, 5000) == jsonLen) {
                        jsonBuf[jsonLen] = 0;
                        Serial.printf("[Server] JSON Received: %s\n", jsonBuf);
                        MyUILogic.handleAICommand(String(jsonBuf)); 
                    }
                    free(jsonBuf);
                }
            }

            // 【关键保护】读完 JSON 强制休息一下，防止 CPU 霸占触发 WDT
            vTaskDelay(pdMS_TO_TICKS(100)); 
            
            // 2. 尝试读取音频长度包头
            Serial.println("[Server] 4G Waiting for Audio Header...");
            if (My4G.readData(lenBuf, 4, 15000) == 4) {
                // 使用 int32_t 检查长度，防止乱码变负数
                int32_t rawLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
                
                // 【核心防火墙】只有长度合法才进入下载循环
                if (rawLen > 0 && rawLen < AUDIO_BUFFER_SIZE) {
                    audioLen = (uint32_t)rawLen;
                    Serial.printf("[Server] 4G Audio Len: %d bytes\n", audioLen);
                    
                    MyUILogic.updateAssistantStatus("接收语音...");
                    
                    size_t totalReceived = 0;
                    size_t chunk = 1024; 
                    bool rxSuccess = true;
                    unsigned long downloadStart = millis();

                    // 下载大数据的安全循环
                    while (totalReceived < audioLen) {
                        // 【看门狗防重启 1】强制喂狗
                        vTaskDelay(pdMS_TO_TICKS(1));

                        // 【看门狗防重启 2】全局下载超时控制（30秒）
                        if (millis() - downloadStart > 30000) {
                            Serial.println("\n[Server] 4G Global Download Timeout!");
                            rxSuccess = false;
                            break;
                        }

                        size_t left = audioLen - totalReceived;
                        size_t toRead = (left > chunk) ? chunk : left;
                        
                        // 这里的 readData 内部必须也包含 vTaskDelay(1)
                        size_t actual = My4G.readData(MyAudio.record_buffer + totalReceived, toRead, 5000);
                        
                        if (actual > 0) {
                            totalReceived += actual;
                            // 每 20KB 打印一个点，进度可见且不阻塞
                            if (totalReceived % 20480 == 0) Serial.print("."); 
                        } else {
                            Serial.println("\n[Server] 4G Read Data Error/Timeout");
                            rxSuccess = false;
                            break;
                        }
                    }

                    if (rxSuccess && totalReceived > 0) {
                        Serial.println("\n[Server] 4G Download Success. Playing...");
                        MyUILogic.updateAssistantStatus("正在回复");
                        // 播放缓冲区里的数据
                        MyAudio.playChunk(MyAudio.record_buffer, totalReceived);
                    }
                } else {
                    // 如果解析出来的长度是天文数字或负数，直接跳过，防止死循环
                    Serial.printf("[Server] 4G Invalid Audio Len: %d, aborting.\n", rawLen);
                }
                
            } else {
                Serial.println("[Server] 4G Audio Length Header Missing");
            }
        } else {
            Serial.println("[Server] 4G Wait JSON Reply Timeout");
        }
    }

    // --- 3. 资源清理 ---
    if (isWiFi) {
        networkClient->stop(); 
    } else {
        My4G.closeTCP();
    }
    
    // 恢复 UI 状态（隐藏助手面板等）
    MyUILogic.finishAIState();
    Serial.println("[Server] Transaction Complete.");
}