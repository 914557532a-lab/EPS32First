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
void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    bool connected = false;

    // 1. 尝试连接服务器
    if (isWiFi) { 
        connected = networkClient->connect(_server_ip, _server_port);
    } else { 
        connected = My4G.connectTCP(_server_ip, _server_port);
    }

    if (!connected) {
        Serial.println("[Server] Connection Failed!");
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(2000);
        if (!isWiFi) My4G.closeTCP();
        MyUILogic.finishAIState();
        return;
    }

    // 2. 发送音频数据
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        // [修改] WiFi 模式：改为发送 4字节大端整数长度 (兼容 Python 脚本)
        uint8_t lenBuf[4];
        lenBuf[0] = (audioSize >> 24) & 0xFF;
        lenBuf[1] = (audioSize >> 16) & 0xFF;
        lenBuf[2] = (audioSize >> 8)  & 0xFF;
        lenBuf[3] = (audioSize)       & 0xFF;
        networkClient->write(lenBuf, 4);
        
        // 发送音频本体
        networkClient->write(MyAudio.record_buffer, audioSize);
        networkClient->flush(); // 确保发出
    } 
    else {
        // 4G 模式发送 (保持不变)
        delay(200);
        if(!sendIntManual(audioSize)) { 
            Serial.println("[4G] Send Len Failed");
            MyUILogic.updateAssistantStatus("发送失败");
            My4G.closeTCP(); 
            MyUILogic.finishAIState();
            return; 
        }
        
        size_t sent = 0;
        while(sent < audioSize) {
            size_t chunk = 1024;
            if(audioSize - sent < chunk) chunk = audioSize - sent;
            if(!My4G.sendData(MyAudio.record_buffer + sent, chunk)) {
                break;
            }
            sent += chunk;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    MyUILogic.updateAssistantStatus("思考中...");

    // 3. 接收响应 (逻辑统一)
    // 定义一些变量用于接收循环
    String jsonHex = "";
    uint32_t startTime = millis();
    bool jsonDone = false;
    
    // 准备音频播放缓冲区
    uint8_t hexPair[2]; 
    int pairIdx = 0;
    uint8_t pcmBuf[512]; 
    int pcmIdx = 0;
    bool audioStarted = false;

    // 进入接收大循环 (超时 45秒)
    while (millis() - startTime < 45000) {
        
        // A. 读取一个字节 (区分 WiFi 和 4G 的读取方式)
        int c = -1;
        if (isWiFi) {
            if (networkClient->connected() && networkClient->available()) {
                c = networkClient->read();
            } else if (!networkClient->connected()) {
                break; // 连接断开
            }
        } else {
            uint8_t temp;
            if (My4G.readData(&temp, 1, 10) == 1) { // 短超时读取
                c = temp;
            }
        }

        // 如果没读到数据，稍作延时继续
        if (c == -1) {
            vTaskDelay(1);
            continue;
        }

        // 重置超时计时器 (只要有数据流过来就不超时)
        startTime = millis();

        // B. 处理数据状态机
        if (!jsonDone) {
            // --- 阶段1: 接收 JSON Hex ---
            if (c == '*') {
                jsonDone = true;
                Serial.println("\n[Protocol] JSON End.");
                
                // 解析并执行指令
                if (jsonHex.length() > 0) {
                    int jLen = jsonHex.length() / 2;
                    char* jBuf = (char*)malloc(jLen + 1);
                    if (jBuf) {
                        for (int i=0; i<jLen; i++) jBuf[i] = (hexCharToVal(jsonHex[i*2]) << 4) | hexCharToVal(jsonHex[i*2+1]);
                        jBuf[jLen] = 0;
                        Serial.printf("[Protocol] JSON: %s\n", jBuf);
                        MyUILogic.handleAICommand(String(jBuf));
                        free(jBuf);
                    }
                }
                
                // 准备进入音频阶段
                Serial.println("[Protocol] Reading Audio...");
                MyUILogic.updateAssistantStatus("正在回复");
                digitalWrite(PIN_PA_EN, HIGH); // 打开功放
                delay(50);
                audioStarted = true;
            } 
            else if (c != '\n' && c != '\r') {
                jsonHex += (char)c;
            }
        } 
        else {
            // --- 阶段2: 接收 Audio Hex ---
            if (c == '*') {
                Serial.println("[Protocol] Audio End (*).");
                break; // 结束整个会话
            }
            
            // 过滤非 Hex 字符
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

            hexPair[pairIdx++] = c;
            if (pairIdx == 2) {
                pcmBuf[pcmIdx++] = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                pairIdx = 0;
                
                if (pcmIdx == 512) {
                    MyAudio.playChunk(pcmBuf, 512);
                    pcmIdx = 0;
                }
            }
        }
    }

    // 播放剩余尾部
    if (pcmIdx > 0) MyAudio.playChunk(pcmBuf, pcmIdx);

    // 关闭功放和连接
    if (audioStarted) {
        delay(50);
        digitalWrite(PIN_PA_EN, LOW);
    }

    if (isWiFi) networkClient->stop();
    else My4G.closeTCP();

    MyUILogic.finishAIState();
    Serial.println("[Server] Done.");
}