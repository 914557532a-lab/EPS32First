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

                    Serial.printf("[Control] Target: %s, Action: %s, Value: %s\n", 
                                  target ? target : "NULL", 
                                  action ? action : "NULL", 
                                  value ? value : "NULL");
                    
                    if (target != NULL) {
                        // =========== 针对空调的控制逻辑 ===========
                        if (strcmp(target, "空调") == 0) {
                            
                            // 1. 解析温度 (默认 26 度)
                            int temp = 26;
                            if (value != NULL && strlen(value) > 0) {
                                temp = atoi(value); // 将字符串 "26" 转为整数
                                // 简单的范围保护
                                if (temp < 16) temp = 16;
                                if (temp > 30) temp = 30;
                            }

                            // 2. 解析开关状态
                            // 默认逻辑：只要有指令(比如"制冷"、"26度")，就认为是开机，除非明确说"关"
                            bool power = true; 
                            
                            if (action != NULL) {
                                if (strcmp(action, "关") == 0) {
                                    power = false;
                                }
                                // 注意：如果 action 是 "开"、"制冷"、"制热"，power 保持为 true
                            }

                            // 3. 调用你的红外控制函数
                            Serial.printf("[Server] 执行空调指令: Power=%d, Temp=%d\n", power, temp);
                            App_IR_Control_AC(power, (uint8_t)temp);
                        } 
                        // =========== 针对灯的控制逻辑 (保留原来的 NEC 测试) ===========
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