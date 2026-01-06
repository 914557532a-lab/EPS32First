#include "App_Server.h"
#include "App_Audio.h"
#include "App_IR.h" // 如果你要控制红外
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
    // 此时 record_buffer 里已经是完整的 WAV 文件（含头）
    client.write(MyAudio.record_buffer, MyAudio.record_data_len);
    client.flush();
    
    // 2. 读取响应头：JSON 长度 (4字节大端)
    // 等待数据
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
    // 解析大端 uint32
    uint32_t json_len = (len_buf[0] << 24) | (len_buf[1] << 16) | (len_buf[2] << 8) | len_buf[3];
    Serial.printf("[Server] JSON Length: %d\n", json_len);

    // 3. 读取 JSON 内容
    // 简单起见，分配一个临时buffer
    char* json_str = (char*)malloc(json_len + 1);
    if (json_str) {
        int read_len = client.readBytes(json_str, json_len);
        json_str[read_len] = 0; // 结尾
        
        Serial.printf("[Server] JSON: %s\n", json_str);
        
        // 解析 JSON (使用 ArduinoJson v6)
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, json_str);
        
            if (!error) {
                const char* reply = doc["reply_text"];
                Serial.printf("[Server] Reply: %s\n", reply); // 调试打印

                // 处理控制指令
                // 注意：JSON 里的 "command" 键对应的结构体在 Python 里是 "control" 下的
                // 请确保 Python 返回的 JSON 结构与这里解析的一致。
                // 根据你的 Python 代码： resp_json = { ..., "control": command_data }
                // 所以这里应该是 doc["control"]
                
                bool has_command = doc["control"]["has_command"];
                if (has_command) {
                    const char* target = doc["control"]["target"]; // "空调"
                    const char* action = doc["control"]["action"]; // "开", "制冷"
                    const char* value  = doc["control"]["value"];  // "26"

                    Serial.printf("[Control] Target: %s, Action: %s, Value: %s\n", target, action, value);
                    
                    // --- 红外指令映射逻辑 (暂时是示例具体的还不知道) ---
                    if (strcmp(target, "空调") == 0) {
                        if (strcmp(action, "开") == 0) {
                            MyIR.sendNEC(0x11111111); // 假设的
                        } else if (strcmp(action, "关") == 0) {
                            MyIR.sendNEC(0x22222222); // 假设的
                        } else if (value != NULL && strcmp(value, "26") == 0) {//温度，目前只支持26°等到测试通过再具体修改这里的函数
                            MyIR.sendNEC(0x33333333); // 假设的
                        }
                    } else if (strcmp(target, "灯") == 0) {
                        if (strcmp(action, "开") == 0) MyIR.sendNEC(0x44444444);
                        else if (strcmp(action, "关") == 0) MyIR.sendNEC(0x55555555);
                    }
                }
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
    MyUILogic.finishAIState(); // <--- 调用你刚才写的函数，恢复按钮和温度显示
    client.stop();
    Serial.println("[Server] Transaction Done.");
}