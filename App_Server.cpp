#include "App_Server.h"
#include "App_Audio.h"
#include "App_IR.h" 
#include "App_UI_Logic.h"

AppServer MyServer;

void AppServer::init(const char* ip, int port) {
    server_ip = ip;
    server_port = port;
}

void AppServer::chatWithServer() {
    if (MyAudio.record_data_len == 0 || MyAudio.record_buffer == NULL) {
        Serial.println("[Server] No audio to send.");
        return;
    }

    WiFiClient client;
    Serial.printf("[Server] Connecting to %s:%d...\n", server_ip, server_port);

    if (!client.connect(server_ip, server_port)) {
        Serial.println("[Server] Connection failed!");
        MyUILogic.finishAIState();
        return;
    }

    // 1. 发送录音数据
    Serial.println("[Server] Sending audio...");
    client.write(MyAudio.record_buffer, MyAudio.record_data_len);
    client.flush();
    
    // 2. 读取响应头：JSON 长度 (4字节大端)
    int timeout = 10000;
    while (client.available() < 4 && timeout > 0) {
        delay(10);
        timeout -= 10;
    }
    
    if (client.available() < 4) {
        Serial.println("[Server] Timeout waiting for response.");
        client.stop();
        return;
    }

    uint8_t len_buf[4];
    client.readBytes(len_buf, 4);
    uint32_t json_len = (len_buf[0] << 24) | (len_buf[1] << 16) | (len_buf[2] << 8) | len_buf[3];
    Serial.printf("[Server] JSON Length: %d\n", json_len);

    // 3. 读取 JSON 内容
    char* json_str = (char*)malloc(json_len + 1);
    if (json_str) {
        int read_len = client.readBytes(json_str, json_len);
        json_str[read_len] = 0; // 结尾
        
        Serial.printf("[Server] JSON: %s\n", json_str);
        
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, json_str);
        
        if (!error) {
            const char* reply = doc["reply_text"];
            if (reply) {
                Serial.printf("[Server] Reply: %s\n", reply); 
            }

            // --- 修复开始：安全地解析 control 指令 ---
            if (doc.containsKey("control")) {
                bool has_command = doc["control"]["has_command"];
                
                if (has_command) {
                    // 获取指针，可能为 NULL
                    const char* target = doc["control"]["target"]; 
                    const char* action = doc["control"]["action"]; 
                    const char* value  = doc["control"]["value"];  

                    // 打印调试信息，处理 NULL 情况
                    Serial.printf("[Control] Target: %s, Action: %s, Value: %s\n", 
                                  target ? target : "NULL", 
                                  action ? action : "NULL", 
                                  value ? value : "NULL");
                    
                    // 【必须修改】在 strcmp 前先检查非空，防止 Core Panic
                    if (target != NULL) {
                        if (strcmp(target, "空调") == 0) {
                            // 检查 action 是否为空
                            if (action != NULL) {
                                if (strcmp(action, "开") == 0) {
                                    MyIR.sendNEC(0x11111111); 
                                } else if (strcmp(action, "关") == 0) {
                                    MyIR.sendNEC(0x22222222); 
                                }
                            }
                            // 检查 value 是否为空
                            if (value != NULL && strcmp(value, "26") == 0) {
                                MyIR.sendNEC(0x33333333); 
                            }
                        } 
                        else if (strcmp(target, "灯") == 0) {
                            if (action != NULL) {
                                if (strcmp(action, "开") == 0) MyIR.sendNEC(0x44444444);
                                else if (strcmp(action, "关") == 0) MyIR.sendNEC(0x55555555);
                            }
                        }
                    } else {
                        Serial.println("[Server] Warning: Command received but 'target' is NULL.");
                    }
                }
            }
            // --- 修复结束 ---

        } else {
            Serial.print("[Server] JSON Deserialize Error: ");
            Serial.println(error.c_str());
        }
        free(json_str);
    }

    // 4. 读取音频长度
    while (client.available() < 4) delay(1);
    client.readBytes(len_buf, 4);
    uint32_t audio_len = (len_buf[0] << 24) | (len_buf[1] << 16) | (len_buf[2] << 8) | len_buf[3];
    Serial.printf("[Server] Audio Length: %d\n", audio_len);

    // 5. 播放音频流
    if (audio_len > 0) {
        MyAudio.playStream(&client, audio_len);
    }
    Serial.println("[Server] Playback finished. Restoring UI.");
    MyUILogic.finishAIState(); 
    client.stop();
    Serial.println("[Server] Transaction Done.");
}