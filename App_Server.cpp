#include "App_Server.h"
#include "App_Audio.h"    
#include "App_UI_Logic.h" 
#include "App_4G.h"       
#include "App_WiFi.h"     
#include "Pin_Config.h"   

AppServer MyServer;

// 硬编码转换，效率高
#define HEX_VAL(c) (((c)>='0'&&(c)<='9') ? (c)-'0' : (((c)>='A'&&(c)<='F') ? (c)-'A'+10 : (((c)>='a'&&(c)<='f') ? (c)-'a'+10 : 0)))

void AppServer::init(const char* ip, uint16_t port) {
    _server_ip = ip;
    _server_port = port;
}

bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24); buf[1] = (val >> 16); buf[2] = (val >> 8); buf[3] = (val);
    return My4G.sendData(buf, 4);
}

void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    bool connected = false;
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

    // 1. 发送录音 (Upload)
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    if (isWiFi) { 
        uint8_t lenBuf[4] = {(uint8_t)((audioSize>>24)&0xFF), (uint8_t)((audioSize>>16)&0xFF), (uint8_t)((audioSize>>8)&0xFF), (uint8_t)(audioSize&0xFF)};
        networkClient->write(lenBuf, 4);
        networkClient->write(MyAudio.record_buffer, audioSize);
        networkClient->flush();
    } else {
        delay(200);
        if(!sendIntManual(audioSize)) { My4G.closeTCP(); MyUILogic.finishAIState(); return; }
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
    
    // 2. 读取 JSON
    String jsonHex = "";
    uint32_t startTime = millis();
    bool jsonDone = false;
    
    while (!jsonDone && millis() - startTime < 10000) {
        int c = -1;
        if (isWiFi) { if (networkClient->available()) c = networkClient->read(); } 
        else { uint8_t b; if (My4G.readData(&b, 1, 50) == 1) c = b; }

        if (c != -1) {
            startTime = millis(); 
            if (c == '*') jsonDone = true;
            else if (c != '\n' && c != '\r') jsonHex += (char)c;
        }
    }

    if (jsonHex.length() > 0) {
        int jLen = jsonHex.length() / 2;
        char* jBuf = (char*)malloc(jLen + 1);
        if (jBuf) {
            for (int i=0; i<jLen; i++) jBuf[i] = (HEX_VAL(jsonHex[i*2]) << 4) | HEX_VAL(jsonHex[i*2+1]);
            jBuf[jLen] = 0;
            Serial.printf("JSON: %s\n", jBuf); 
            MyUILogic.handleAICommand(String(jBuf));
            free(jBuf);
        }
    }

    // =================================================================================
    // 3. 全缓冲下载模式 (Download All to RAM)
    // =================================================================================
    Serial.println("Start Download..."); 
    MyUILogic.updateAssistantStatus("正在接收...");

    // [修复] 变量声明提前，防止 goto 跳过初始化
    uint32_t totalDownloaded = 0;
    uint32_t lastDataTime = 0;
    
    // 申请 1.5MB PSRAM
    uint32_t bufSize = 1024 * 1536; 
    char* bigHexBuf = (char*)ps_malloc(bufSize);
    
    if (!bigHexBuf) {
        Serial.println("ERR: PSRAM OOM");
        goto _EXIT_CLEANUP;
    }

    startTime = millis();
    lastDataTime = millis();

    // --- 下载循环 ---
    while (millis() - startTime < 60000) { 
        int readCount = 0;
        int chunkSize = 2048; 
        
        if (totalDownloaded + chunkSize >= bufSize) chunkSize = bufSize - totalDownloaded - 1;

        if (isWiFi) {
            if (networkClient->connected() && networkClient->available()) 
                readCount = networkClient->read((uint8_t*)bigHexBuf + totalDownloaded, chunkSize);
            else if (!networkClient->connected()) break; 
        } else {
            readCount = My4G.readData((uint8_t*)bigHexBuf + totalDownloaded, chunkSize, 100); 
        }

        if (readCount > 0) {
            lastDataTime = millis();
            bool foundEnd = false;
            for(int i=0; i<readCount; i++) {
                if (bigHexBuf[totalDownloaded + i] == '*') {
                    foundEnd = true;
                    readCount = i; 
                    break;
                }
            }
            
            totalDownloaded += readCount;
            if (foundEnd) {
                Serial.println("Download End (*)");
                break;
            }
        } else {
             if (!isWiFi && (millis() - lastDataTime > 3000)) {
                 Serial.print("."); 
                 My4G.sendRawAT("AT+MIPREAD=1,3072"); 
                 lastDataTime = millis();
             }
             vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    Serial.printf("Download Size: %d bytes\n", totalDownloaded);

    if (isWiFi) networkClient->stop();
    else My4G.closeTCP();
    Serial.println("Net Closed. Start Play...");

    // =================================================================================
    // 4. 本地解码播放 (Local Playback)
    // =================================================================================
    if (totalDownloaded > 0) {
        MyUILogic.updateAssistantStatus("正在回复");
        MyAudio.streamStart();

        // 临时 PCM 缓冲 (1KB)
        const int PCM_CHUNK_SIZE = 1024;
        uint8_t* pcmBuf = (uint8_t*)malloc(PCM_CHUNK_SIZE);
        
        if (pcmBuf) {
            int pcmIdx = 0;
            uint8_t hexPair[2];
            int pairIdx = 0;
            
            for (uint32_t i = 0; i < totalDownloaded; i++) {
                char c = bigHexBuf[i];
                
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

                hexPair[pairIdx++] = c;
                if (pairIdx == 2) {
                    pairIdx = 0;
                    pcmBuf[pcmIdx++] = (HEX_VAL(hexPair[0]) << 4) | HEX_VAL(hexPair[1]);
                    
                    if (pcmIdx >= PCM_CHUNK_SIZE) {
                        MyAudio.streamWrite(pcmBuf, pcmIdx);
                        pcmIdx = 0;
                    }
                }
            }
            if (pcmIdx > 0) MyAudio.streamWrite(pcmBuf, pcmIdx);
            free(pcmBuf);
        }
        MyAudio.streamEnd();
        Serial.println("Play Done");
    }

    if (bigHexBuf) free(bigHexBuf);

_EXIT_CLEANUP:
    if (isWiFi) networkClient->stop();
    // 4G 已在前面关闭

    MyUILogic.finishAIState();
}