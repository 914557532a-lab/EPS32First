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

    // [错误处理] 如果连接失败，明确报错并退出，防止 UI 卡死
    if (!connected) {
        Serial.println("[Server] Connection Failed!");
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(2000); // 停留一下让用户看清提示
        
        if (!isWiFi) My4G.closeTCP(); // 确保清理连接
        MyUILogic.finishAIState();    // 退出 AI 界面
        return;
    }

    // 2. 发送音频数据
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        // WiFi 模式发送
        networkClient->print(String(audioSize) + "\n");
        networkClient->write(MyAudio.record_buffer, audioSize);
    } 
    else {
        // 4G 模式发送
        delay(200);
        // 先发送长度
        if(!sendIntManual(audioSize)) { 
            Serial.println("[4G] Send Len Failed");
            MyUILogic.updateAssistantStatus("发送失败");
            My4G.closeTCP(); 
            MyUILogic.finishAIState();
            return; 
        }
        
        // 分块发送音频数据
        size_t sent = 0;
        while(sent < audioSize) {
            size_t chunk = 1024;
            if(audioSize - sent < chunk) chunk = audioSize - sent;
            
            if(!My4G.sendData(MyAudio.record_buffer + sent, chunk)) {
                Serial.println("[4G] Send Data Failed");
                break; // 发送中断
            }
            sent += chunk;
            vTaskDelay(pdMS_TO_TICKS(5)); // 防止看门狗复位
        }
    }
    
    MyUILogic.updateAssistantStatus("思考中...");

    // 3. 接收响应
    if (!isWiFi) {
        // ==================== 4G 接收逻辑 ====================
        
        // (A) 接收 JSON 文本
        Serial.println("[4G] Reading JSON...");
        String jsonHex = "";
        uint32_t startTime = millis();
        
        while (millis() - startTime < 15000) { 
            uint8_t c;
            // 每次读 1 个字节
            if (My4G.readData(&c, 1, 100) == 1) { 
                if (c == '*') break; // 遇到星号结束 JSON 部分
                if (c != '\n' && c != '\r') jsonHex += (char)c; 
                startTime = millis(); 
            }
            vTaskDelay(1);
        }

        // 解析并执行 JSON 指令
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
        } else {
             Serial.println("[4G] No JSON Response or Timeout");
        }

        // (B) 接收音频流
        Serial.println("[4G] Reading Audio...");
        MyUILogic.updateAssistantStatus("正在回复");
        
        // [核心修复] 播放开始前：打开功放
        // 必须在这里打开，而不是在 App_Audio 里频繁开关
        digitalWrite(PIN_PA_EN, HIGH);
        delay(50); // 防爆破音预热

        uint8_t hexPair[2]; 
        int pairIdx = 0;
        uint8_t pcmBuf[512]; 
        int pcmIdx = 0;
        
        startTime = millis();
        // 音频接收循环
        while (millis() - startTime < 30000) { 
            uint8_t c;
            if (My4G.readData(&c, 1, 100) == 1) {
                startTime = millis(); 
                
                if (c == '*') { // 遇到星号结束整个传输
                    Serial.println("[4G] Audio End (*).");
                    break; 
                }
                
                // 过滤非 Hex 字符
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

                // 拼凑 Hex 对
                hexPair[pairIdx++] = c;
                if (pairIdx == 2) {
                    pcmBuf[pcmIdx++] = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                    pairIdx = 0;
                    
                    // 缓冲区满，播放一段
                    if (pcmIdx == 512) {
                        MyAudio.playChunk(pcmBuf, 512);
                        pcmIdx = 0;
                        // 注意：不要在这里加 delay，否则声音会卡
                    }
                }
            } else {
                vTaskDelay(1);
            }
        }
        // 播放剩余的尾部音频
        if (pcmIdx > 0) MyAudio.playChunk(pcmBuf, pcmIdx);

        // [核心修复] 播放结束后：关闭功放
        delay(50); // 等最后一点余音播完
        digitalWrite(PIN_PA_EN, LOW);

        // 关闭 4G TCP 连接
        My4G.closeTCP();
    } 
    else { 
        // ==================== WiFi 接收逻辑 ====================
        // (如果是 WiFi 模式，记得也要检查是否有类似的 PA 控制逻辑)
        // 这里仅作为示例保留关闭连接
        networkClient->stop(); 
    } 

    // 4. 结束会话，恢复 UI
    MyUILogic.finishAIState();
    Serial.println("[Server] Done.");
}