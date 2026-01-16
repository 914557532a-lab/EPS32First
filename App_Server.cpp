#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     
#include "Pin_Config.h"   

AppServer MyServer;

void AppServer::init(const char* ip, uint16_t port) {
    _server_ip = ip;
    _server_port = port;
}

// [辅助函数] 从网络读取指定长度的原始字节
// 兼容 WiFiClient 和 App4G，带有超时重试机制，确保读够 len 个字节
bool readBytesFixed(Client* client, bool isWiFi, uint8_t* buf, size_t len, uint32_t timeout_ms) {
    size_t received = 0;
    uint32_t start = millis();
    
    while (received < len && (millis() - start < timeout_ms)) {
        int n = 0;
        if (isWiFi) {
            if (client->available()) {
                // WiFiClient 的 read(buf, len) 返回实际读取字节数
                n = client->read(buf + received, len - received);
            } else {
                // 等待数据到达，避免空转
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else {
            // 4G 模块读取 (App4G 内部实现了缓冲和 AT 指令读取)
            // 每次请求适量数据，防止底层 buffer 溢出或超时
            size_t chunk = len - received;
            if (chunk > 512) chunk = 512; 
            
            // 调用 App4G 的 readData，timeout 设为短时间，由外层循环控制总超时
            n = My4G.readData(buf + received, chunk, 50); 
        }

        if (n > 0) {
            received += n;
            start = millis(); // 每次读到数据都重置超时，适应慢速网络
        } else {
             // 如果没读到数据，短暂延时
             vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    if (received < len) {
        Serial.printf("[Server] Read Timeout/Fail. Want: %d, Got: %d\n", len, received);
        return false;
    }
    return true;
}

// [辅助函数] 发送原始数据
bool sendDataFixed(Client* client, bool isWiFi, const uint8_t* buf, size_t len) {
    if (isWiFi) {
        size_t sent = 0;
        while (sent < len) {
            size_t chunk = 1024;
            if (len - sent < chunk) chunk = len - sent;
            if (client->write(buf + sent, chunk) != chunk) return false;
            sent += chunk;
            client->flush(); 
        }
        return true;
    } else {
        // 4G 发送
        size_t sent = 0;
        while (sent < len) {
            // 4G 模块建议分块发送，避免 AT 指令缓冲区溢出
            size_t chunk = 512; 
            if (len - sent < chunk) chunk = len - sent;
            if (!My4G.sendData(buf + sent, chunk)) return false;
            sent += chunk;
        }
        return true;
    }
}

void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    
    // [修复] 变量声明全部移到函数开头，防止 goto 跳过初始化导致编译错误
    uint32_t audioSize = 0;
    uint8_t lenBuf[4] = {0};
    uint32_t jsonLen = 0;
    uint32_t audioLen = 0;
    char* jsonBuf = NULL;
    uint8_t* downBuf = NULL;
    bool connected = false;

    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    if (isWiFi) connected = networkClient->connect(_server_ip, _server_port);
    else connected = My4G.connectTCP(_server_ip, _server_port);

    if (!connected) {
        Serial.println("ERR: Conn Fail"); 
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        if (!isWiFi) My4G.closeTCP(); 
        MyUILogic.finishAIState();    
        return;
    }

    // =================================================================================
    // 1. 发送录音 (Protocol: [Len 4B] + [Data])
    // =================================================================================
    audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    Serial.printf("[Server] Uploading %d bytes...\n", audioSize);

    // 1.1 发送长度 (4字节大端序，匹配 Python 的 struct.unpack('>I'))
    lenBuf[0] = (uint8_t)((audioSize >> 24) & 0xFF);
    lenBuf[1] = (uint8_t)((audioSize >> 16) & 0xFF);
    lenBuf[2] = (uint8_t)((audioSize >> 8) & 0xFF);
    lenBuf[3] = (uint8_t)(audioSize & 0xFF);

    if (!sendDataFixed(networkClient, isWiFi, lenBuf, 4)) goto _EXIT_ERROR;

    // 1.2 发送音频实体
    if (!sendDataFixed(networkClient, isWiFi, MyAudio.record_buffer, audioSize)) goto _EXIT_ERROR;

    MyUILogic.updateAssistantStatus("思考中...");
    
    // =================================================================================
    // 2. 接收 JSON (Protocol: [Len 4B] + [Data])
    // =================================================================================
    // 2.1 读取 JSON 长度
    // 给予较长超时 (15s)，因为 AI 分析和生成需要时间
    if (!readBytesFixed(networkClient, isWiFi, lenBuf, 4, 15000)) goto _EXIT_ERROR; 
    
    // 解析大端序长度
    jsonLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
    Serial.printf("[Server] JSON Len: %d\n", jsonLen);

    // 2.2 读取 JSON 数据
    if (jsonLen > 0 && jsonLen < 32768) { // 安全限制
        jsonBuf = (char*)malloc(jsonLen + 1);
        if (!jsonBuf) { Serial.println("ERR: OOM JSON"); goto _EXIT_ERROR; }
        
        if (!readBytesFixed(networkClient, isWiFi, (uint8_t*)jsonBuf, jsonLen, 5000)) {
            goto _EXIT_ERROR; // 标签处会释放 jsonBuf
        }
        jsonBuf[jsonLen] = 0; // 字符串结束符
        Serial.printf("[Server] JSON: %s\n", jsonBuf);
        
        // 执行指令逻辑
        MyUILogic.handleAICommand(String(jsonBuf));
        free(jsonBuf); jsonBuf = NULL; // 用完及时释放
    } else if (jsonLen > 0) {
        Serial.println("ERR: JSON too large, skip.");
        // 如果 JSON 太大，这里最好断开连接，因为很难精准跳过
        goto _EXIT_ERROR; 
    }

    // =================================================================================
    // 3. 接收音频 (Protocol: [Len 4B] + [Data])
    // =================================================================================
    MyUILogic.updateAssistantStatus("正在接收...");
    
    // 3.1 读取 Audio 长度
    if (!readBytesFixed(networkClient, isWiFi, lenBuf, 4, 5000)) goto _EXIT_ERROR;
    
    audioLen = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
    Serial.printf("[Server] Audio Len: %d\n", audioLen);

    if (audioLen > 0) {
        // 3.2 接收音频数据
        // 策略：直接下载到 PSRAM (bufferAll) 然后播放。
        downBuf = (uint8_t*)ps_malloc(audioLen);
        if (!downBuf) {
            Serial.println("ERR: PSRAM OOM for Audio");
            goto _EXIT_ERROR;
        }
        
        uint32_t tStart = millis();
        // 动态计算超时：假设最差网速 5KB/s，加上 5秒 基础时间
        uint32_t dlTimeout = (audioLen / 1024) * 200 + 8000; 
        
        Serial.printf("[Server] Downloading... (Timeout: %d ms)\n", dlTimeout);

        if (readBytesFixed(networkClient, isWiFi, downBuf, audioLen, dlTimeout)) {
            Serial.printf("[Server] Download OK. Time: %d ms, Speed: %d KB/s\n", 
                millis() - tStart, 
                (millis() - tStart) > 0 ? (audioLen / (millis() - tStart)) : 0);
            
            // 下载完成，尽早断开连接释放资源
            if (isWiFi) networkClient->stop();
            else My4G.closeTCP();
            
            MyUILogic.updateAssistantStatus("正在回复");
            
            // 3.3 播放
            MyAudio.streamStart();
            
            // 分块写入音频流
            size_t written = 0;
            const size_t CHUNK = 512; 
            while(written < audioLen) {
                size_t w = (audioLen - written > CHUNK) ? CHUNK : (audioLen - written);
                
                // 写入环形缓冲区 (现在是阻塞式的，不会丢数据)
                MyAudio.streamWrite(downBuf + written, w); 
                written += w;
                
                // 简单流控
                vTaskDelay(pdMS_TO_TICKS(2)); 
            }
            
            MyAudio.streamEnd();
            Serial.println("[Server] Play Done");
            
        } else {
            Serial.println("ERR: Download Timeout");
        }
        
        if (downBuf) free(downBuf); downBuf = NULL;
    }

    // 正常结束
    if (isWiFi) networkClient->stop();
    else My4G.closeTCP(); // 确保关闭
    MyUILogic.finishAIState();
    return;

_EXIT_ERROR:
    Serial.println("ERR: Transaction Failed");
    if (jsonBuf) free(jsonBuf); // 安全释放
    if (downBuf) free(downBuf);
    if (isWiFi) networkClient->stop();
    else My4G.closeTCP();
    MyUILogic.finishAIState();
}