#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     

AppServer MyServer;

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

bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24); buf[1] = (val >> 16); buf[2] = (val >> 8); buf[3] = (val);
    return My4G.sendData(buf, 4);
}

// =================================================================================
// 核心逻辑：星号协议版 (*)
// =================================================================================
void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    if (isWiFi) { if (!networkClient->connect(_server_ip, _server_port)) return; } 
    else { if (!My4G.connectTCP(_server_ip, _server_port)) return; }

    // 1. 发送 (不变)
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    if (isWiFi) { /*WiFi发送略*/ } 
    else {
        delay(200);
        if(!sendIntManual(audioSize)) { My4G.closeTCP(); return; }
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

    // 2. 接收响应 (查找 '*' 结束符)
    if (!isWiFi) {
        // (A) JSON
        Serial.println("[4G] Reading JSON...");
        String jsonHex = "";
        uint32_t startTime = millis();
        
        while (millis() - startTime < 15000) { 
            uint8_t c;
            // 每次读1个字节
            if (My4G.readData(&c, 1, 100) == 1) { 
                if (c == '*') break; // 【关键】遇到星号结束
                if (c != '\n' && c != '\r') jsonHex += (char)c; // 忽略换行符干扰
                startTime = millis(); 
            }
            vTaskDelay(1);
        }

        if (jsonHex.length() > 0) {
            Serial.printf("[4G] JSON Hex Len: %d\n", jsonHex.length());
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

        // (B) Audio
        Serial.println("[4G] Reading Audio...");
        MyUILogic.updateAssistantStatus("正在回复");
        
        uint8_t hexPair[2]; 
        int pairIdx = 0;
        uint8_t pcmBuf[512]; 
        int pcmIdx = 0;
        
        startTime = millis();
        while (millis() - startTime < 30000) { 
            uint8_t c;
            if (My4G.readData(&c, 1, 100) == 1) {
                startTime = millis(); 
                
                if (c == '*') { // 【关键】遇到星号结束
                    Serial.println("[4G] Audio End (*).");
                    break; 
                }
                
                // 过滤非Hex字符（增强鲁棒性）
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

                hexPair[pairIdx++] = c;
                if (pairIdx == 2) {
                    pcmBuf[pcmIdx++] = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                    pairIdx = 0;
                    if (pcmIdx == 512) {
                        MyAudio.playChunk(pcmBuf, 512);
                        pcmIdx = 0;
                        vTaskDelay(1);
                    }
                }
            } else {
                vTaskDelay(1);
            }
        }
        if (pcmIdx > 0) MyAudio.playChunk(pcmBuf, pcmIdx);
    } 
    else { networkClient->stop(); } // WiFi处理忽略

    if (!isWiFi) My4G.closeTCP();
    MyUILogic.finishAIState();
    Serial.println("[Server] Done.");
}