#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     
#include "Pin_Config.h"   // [关键] 必须引入这个才能控制功放引脚

AppServer MyServer;

// 辅助函数：Hex字符转数值
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

// 辅助函数：手动发送整数（用于 4G 模式）
bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24); buf[1] = (val >> 16); buf[2] = (val >> 8); buf[3] = (val);
    return My4G.sendData(buf, 4);
}

// =================================================================================
// 核心逻辑：星号协议版 (*)
// =================================================================================

// [文件] App_Server.cpp

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
        vTaskDelay(2000); 
        if (!isWiFi) My4G.closeTCP(); 
        MyUILogic.finishAIState();    
        return;
    }

    // --- 发送音频 ---
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        networkClient->print(String(audioSize) + "\n");
        networkClient->write(MyAudio.record_buffer, audioSize);
    } else {
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

    // --- 接收响应 ---
    if (!isWiFi) {
        // 1. 接收 JSON
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

        // 2. 接收音频 (关键修复：使用 malloc)
        Serial.println("[4G] Reading Audio...");
        MyUILogic.updateAssistantStatus("正在回复");
        
        digitalWrite(PIN_PA_EN, HIGH);
        delay(50); 

        const int BUF_SIZE = 4096; 
        // [核心修复] 改为动态分配，防止栈溢出
        uint8_t* pcmBuf = (uint8_t*)malloc(BUF_SIZE);
        
        if (pcmBuf) { // 只有分配成功才执行
            int pcmIdx = 0;
            uint8_t hexPair[2]; 
            int pairIdx = 0;
            int idleCount = 0;

            startTime = millis();
            while (millis() - startTime < 30000) { 
                uint8_t c;
                // [优化] 增加超时检测稳定性
                if (My4G.readData(&c, 1, 20) == 1) {
                    startTime = millis(); 
                    idleCount = 0;
                    
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
            
            // [核心修复] 必须释放内存！
            free(pcmBuf); 
        } else {
            Serial.println("[Server] Alloc Failed!");
        }

        delay(50); 
        digitalWrite(PIN_PA_EN, LOW);
        My4G.closeTCP();
    } else { 
        networkClient->stop(); 
    } 

    MyUILogic.finishAIState();
    Serial.println("[Server] Done.");
}