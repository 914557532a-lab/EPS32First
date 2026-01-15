#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     
#include "Pin_Config.h"   

AppServer MyServer;

// 辅助工具：Hex字符转数值
static uint8_t hexCharToVal(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

void AppServer::init(const char* ip, uint16_t port) {
    _server_ip = ip;
    _server_port = port;
}

// 辅助工具：4G模式下手动发送整数
bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24); buf[1] = (val >> 16); buf[2] = (val >> 8); buf[3] = (val);
    return My4G.sendData(buf, 4);
}

// =================================================================================
// 核心逻辑：WiFi 极速批量下载流式版 (解决卡顿的终极方案)
// =================================================================================

void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    bool connected = false;
    if (isWiFi) connected = networkClient->connect(_server_ip, _server_port);
    else connected = My4G.connectTCP(_server_ip, _server_port);

    if (!connected) {
        Serial.println("[Server] Connection Failed!");
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        if (!isWiFi) My4G.closeTCP(); 
        MyUILogic.finishAIState();    
        return;
    }

    // ================= 1. 发送音频 =================
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        // WiFi: 发送4字节大端整数长度
        uint8_t lenBuf[4];
        lenBuf[0] = (audioSize >> 24) & 0xFF;
        lenBuf[1] = (audioSize >> 16) & 0xFF;
        lenBuf[2] = (audioSize >> 8) & 0xFF;
        lenBuf[3] = (audioSize) & 0xFF;
        networkClient->write(lenBuf, 4);
        
        // 发送录音数据
        networkClient->write(MyAudio.record_buffer, audioSize);
        networkClient->flush();
    } else {
        // 4G: 手动发送 (保持原样)
        delay(200);
        if(!sendIntManual(audioSize)) { 
            My4G.closeTCP(); MyUILogic.finishAIState(); return; 
        }
        size_t sent = 0;
        while(sent < audioSize) {
            size_t chunk = 1024;
            if(audioSize - sent < chunk) chunk = audioSize - sent;
            if(!My4G.sendData(MyAudio.record_buffer + sent, chunk)) break;
            sent += chunk;
            vTaskDelay(pdMS_TO_TICKS(5)); 
        }
    }
    
    MyUILogic.updateAssistantStatus("思考中...");

    // ================= 2. 接收响应 =================
    
    if (isWiFi) {
        networkClient->setTimeout(10000); 

        // (A) 接收 JSON
        Serial.println("[WiFi] Reading JSON...");
        String jsonHex = "";
        uint32_t startTime = millis();
        
        while (networkClient->connected() && millis() - startTime < 10000) {
            if (networkClient->available()) {
                char c = networkClient->read();
                if (c == '*') break; // JSON 结束符
                if (c != '\n' && c != '\r') jsonHex += c;
                startTime = millis(); 
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }

        if (jsonHex.length() > 0) {
            int jLen = jsonHex.length() / 2;
            char* jBuf = (char*)malloc(jLen + 1);
            if (jBuf) {
                for (int i=0; i<jLen; i++) jBuf[i] = (hexCharToVal(jsonHex[i*2]) << 4) | hexCharToVal(jsonHex[i*2+1]);
                jBuf[jLen] = 0;
                Serial.printf("[WiFi] JSON: %s\n", jBuf);
                MyUILogic.handleAICommand(String(jBuf));
                free(jBuf);
            }
        }

        // (B) 接收音频流 (极速批量下载版)
        Serial.println("[WiFi] Fast Stream Start...");
        MyUILogic.updateAssistantStatus("正在回复");

        MyAudio.streamStart();
        
        uint8_t hexPair[2];
        int pairIdx = 0;
        
        // 1. 网络读取缓冲 (越大越快，ESP32S3 SRAM 很大，给 2KB)
        const int NET_READ_SIZE = 2048;
        uint8_t* netBuf = (uint8_t*)malloc(NET_READ_SIZE);

        // 2. 音频写入缓冲
        const int AUDIO_WRITE_SIZE = 1024;
        uint8_t* audioChunk = (uint8_t*)malloc(AUDIO_WRITE_SIZE);
        int audioIdx = 0;

        if (!netBuf || !audioChunk) {
            Serial.println("[Server] ERR: OOM");
            goto _END_WIFI;
        }

        startTime = millis();
        
        while (networkClient->connected() && millis() - startTime < 30000) {
            int avail = networkClient->available();
            if (avail > 0) {
                startTime = millis(); // 有数据就刷新超时
                
                // [核心优化] 批量从网络栈读取数据，不再是一个个字节读
                // 这行代码是提速的关键：一次拉取最多 2KB
                int toRead = (avail > NET_READ_SIZE) ? NET_READ_SIZE : avail;
                int readLen = networkClient->read(netBuf, toRead);

                // 本地极速循环处理
                for (int i = 0; i < readLen; i++) {
                    char c = (char)netBuf[i];

                    if (c == '*') { 
                        Serial.println("[WiFi] Stream End (*)."); 
                        goto _END_STREAM_LOOP; // 跳出多层循环
                    } 

                    // 快速过滤
                    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

                    hexPair[pairIdx++] = c;
                    if (pairIdx == 2) {
                        uint8_t val = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                        pairIdx = 0;
                        
                        audioChunk[audioIdx++] = val;
                        
                        // 积攒一小块推送到播放引擎，减少函数调用开销
                        if (audioIdx == AUDIO_WRITE_SIZE) {
                            MyAudio.streamWrite(audioChunk, audioIdx);
                            audioIdx = 0;
                        }
                    }
                }
                // 批量读取不需要 delay，全力下载
            } else {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }

_END_STREAM_LOOP:
        // 推送剩余尾部数据
        if (audioIdx > 0) {
            MyAudio.streamWrite(audioChunk, audioIdx);
        }

        MyAudio.streamEnd();
        Serial.println("[WiFi] Download Done.");

_END_WIFI:
        if (netBuf) free(netBuf);
        if (audioChunk) free(audioChunk);
        networkClient->stop();

    } else {
        // [4G 逻辑保持原样，未做修改]
        Serial.println("[4G] Reading JSON...");
        String jsonHex = "";
        uint32_t startTime = millis();
        while (millis() - startTime < 15000) { 
            uint8_t c;
            if (My4G.readData(&c, 1, 50) == 1) { 
                if (c == '*') break; 
                if (c != '\n' && c != '\r') jsonHex += (char)c; 
                startTime = millis(); 
            } else vTaskDelay(1);
        }
        if (jsonHex.length() > 0) {
            int jLen = jsonHex.length() / 2;
            char* jBuf = (char*)malloc(jLen + 1);
            if (jBuf) {
                for (int i=0; i<jLen; i++) jBuf[i] = (hexCharToVal(jsonHex[i*2]) << 4) | hexCharToVal(jsonHex[i*2+1]);
                jBuf[jLen] = 0;
                Serial.printf("[4G] JSON: %s\n", jBuf);
                MyUILogic.handleAICommand(String(jBuf));
                free(jBuf);
            }
        }
        Serial.println("[4G] Reading Audio...");
        MyUILogic.updateAssistantStatus("正在回复");
        digitalWrite(PIN_PA_EN, HIGH); 
        delay(50); 
        const int BUF_SIZE = 4096; 
        uint8_t* pcmBuf = (uint8_t*)malloc(BUF_SIZE);
        if (pcmBuf) { 
            int pcmIdx = 0; uint8_t hexPair[2]; int pairIdx = 0; int idleCount = 0;
            startTime = millis();
            while (millis() - startTime < 30000) { 
                uint8_t c;
                if (My4G.readData(&c, 1, 20) == 1) {
                    startTime = millis(); idleCount = 0;
                    if (c == '*') { Serial.println("[4G] Audio End (*)."); break; } 
                    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;
                    hexPair[pairIdx++] = c;
                    if (pairIdx == 2) {
                        pcmBuf[pcmIdx++] = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                        pairIdx = 0;
                        if (pcmIdx == BUF_SIZE) {
                            MyAudio.playChunk(pcmBuf, BUF_SIZE);
                            pcmIdx = 0;
                        }
                    }
                } else {
                    idleCount++;
                    if (idleCount > 50) vTaskDelay(1); 
                }
            }
            if (pcmIdx > 0) MyAudio.playChunk(pcmBuf, pcmIdx);
            free(pcmBuf); 
        }
        Serial.println("[4G] Waiting for audio drain...");
        delay(1500); 
        digitalWrite(PIN_PA_EN, LOW); 
        My4G.closeTCP();
    } 

    MyUILogic.finishAIState();
    Serial.println("[Server] Done.");
}