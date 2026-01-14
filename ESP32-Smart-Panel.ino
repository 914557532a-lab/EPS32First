/**
 * @file ESP32-Smart-Panel.ino
 */

#include <Arduino.h>
#include <FreeRTOS.h>
#include <math.h>
#include <HTTPClient.h>
#include "Pin_Config.h"
#include "App_Sys.h"
#include "App_Display.h"
#include "App_UI_Logic.h" 
#include "App_Audio.h"
#include "App_WiFi.h"
#include "App_4G.h"
#include "App_IR.h"
#include "App_Server.h"
#include "App_433.h"

// =========================================================================
//  SERVER CONFIGURATION (在此处修改服务器地址)
// =========================================================================

// --- 方案 A: 你的 cpolar 公网地址 (车载/远程使用) ---
// 注意：如果 cpolar 是 HTTP 隧道，端口通常是 80；如果是 TCP 隧道，端口是分配的随机端口
#define SERVER_HOST  "1.tcp.cpolar.top"
#define SERVER_PORT  11693 

// --- 方案 B: 本地局域网地址 (开发/调试使用) ---
// #define SERVER_HOST  "192.168.1.53"
// #define SERVER_PORT  8080

// =========================================================================

volatile float g_SystemTemp = 0.0f;
QueueHandle_t AudioQueue_Handle = NULL;
QueueHandle_t KeyQueue_Handle   = NULL;
QueueHandle_t IRQueue_Handle    = NULL;
QueueHandle_t NetQueue_Handle   = NULL;

TaskHandle_t TaskUI_Handle    = NULL;
TaskHandle_t TaskSys_Handle   = NULL;
TaskHandle_t TaskAudio_Handle = NULL;
TaskHandle_t TaskNet_Handle   = NULL;
TaskHandle_t TaskIR_Handle    = NULL;
TaskHandle_t Task433_Handle   = NULL;

// 定义网络工作模式
enum NetMode {
    NET_MODE_AUTO,      // 默认：优先 WiFi，断开自动切 4G
    NET_MODE_WIFI_ONLY, // 强制 WiFi，不使用 4G
    NET_MODE_4G_ONLY    // 强制 4G，主动关闭 WiFi (适合车载测试)
};

// 当前网络模式 (默认为 自动)
NetMode currentNetMode = NET_MODE_AUTO;

struct AudioMsg {
    uint8_t type; 
    int param;
};

// ================= [Core 1] TaskUI =================
void TaskUI_Code(void *pvParameters) {
    MyDisplay.init();
    MyUILogic.init();
    KeyAction keyMsg;
    for(;;) {
        lv_tick_inc(5);
        if (xQueueReceive(KeyQueue_Handle, &keyMsg, 0) == pdTRUE) {
            Serial.printf("[UI] Key Received: %d\n", keyMsg);
            MyUILogic.handleInput(keyMsg);
        }
        MyDisplay.loop();
        MyUILogic.loop();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ================= [Core 1] TaskSys =================
void TaskSys_Code(void *pvParameters) {
    MySys.init();
    static uint32_t lastTempTime = 0;
    for(;;) {
        KeyAction action = MySys.getKeyAction();
        if (action != KEY_NONE) {
            xQueueSend(KeyQueue_Handle, &action, 0);
        }
        if (millis() - lastTempTime > 1000) {
            lastTempTime = millis();
            g_SystemTemp = MySys.getTemperatureC();
        }
        MySys.scanLoop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ================= [Core 0] TaskAudio =================
void TaskAudio_Code(void *pvParameters) {
    MyAudio.init();
    Serial.println("[Audio] Initialized (Muted).");
    AudioMsg msg;
    for(;;) {
        if (xQueueReceive(AudioQueue_Handle, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.type) {
                case 0: MyAudio.playToneAsync(800, 200); break;
                case 1: MyAudio.startRecording(); break;
                case 2: MyAudio.stopRecording(); break;
            }
        }
    }
}

// ================= [Core 0] TaskNet =================
// 负责网络连接维持 (WiFi/4G) 和 数据上传下载
void TaskNet_Code(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 1. 初始化 WiFi (但根据模式决定是否连接)
    MyWiFi.init();
    if (currentNetMode != NET_MODE_4G_ONLY) {
        MyWiFi.connect("HC-2G", "aa888888");
    } else {
        Serial.println("[Net] Mode is 4G ONLY. Skipping WiFi connect.");
        WiFi.mode(WIFI_OFF);
    }
    
    // 2. 初始化 4G
    My4G.init();
    My4G.powerOn(); // 4G 总是上电待命，或者你可以优化为按需上电

    // 3. 初始化服务器地址
    MyServer.init(SERVER_HOST, SERVER_PORT);
    
    WiFiClient wifiClient; 
    NetMessage msg;
    
    for(;;) {
        // --- [修改] 串口指令监听 ---
        // 支持: 
        // 1. "AT+..." -> 发给 4G 模块
        // 2. "NET=4G" -> 强制切断 WiFi，只用 4G
        // 3. "NET=WIFI" -> 恢复 WiFi
        // 4. "NET=AUTO" -> 自动模式
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            if (input.length() > 0) {
                if (input.startsWith("NET=")) {
                    String modeStr = input.substring(4);
                    modeStr.toUpperCase();
                    if (modeStr == "4G") {
                        currentNetMode = NET_MODE_4G_ONLY;
                        WiFi.disconnect(true);
                        WiFi.mode(WIFI_OFF);
                        Serial.println("\n>>> CMD: Switched to [4G ONLY] Mode. WiFi OFF. <<<");
                        MyUILogic.updateAssistantStatus("网络: 仅4G");
                    } 
                    else if (modeStr == "WIFI") {
                        currentNetMode = NET_MODE_WIFI_ONLY;
                        WiFi.mode(WIFI_STA);
                        MyWiFi.connect("HC-2G", "aa888888");
                        Serial.println("\n>>> CMD: Switched to [WiFi ONLY] Mode. <<<");
                        MyUILogic.updateAssistantStatus("网络: 仅WiFi");
                    }
                    else if (modeStr == "AUTO") {
                        currentNetMode = NET_MODE_AUTO;
                        if (!MyWiFi.isConnected()) {
                            WiFi.mode(WIFI_STA);
                            MyWiFi.connect("HC-2G", "aa888888");
                        }
                        Serial.println("\n>>> CMD: Switched to [AUTO] Mode. <<<");
                        MyUILogic.updateAssistantStatus("网络: 自动");
                    }
                } else {
                    // 默认当作 AT 指令发给 4G
                    My4G.sendRawAT(input);
                }
            }
        }

        // --- 1. 处理消息队列 ---
        if (xQueueReceive(NetQueue_Handle, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            
            if (msg.type == NET_EVENT_UPLOAD_AUDIO) {
                Serial.println("[Net] Upload Request Received.");
                Client* activeClient = NULL;
                
                // 根据模式选择网络
                bool allowWiFi = (currentNetMode == NET_MODE_AUTO || currentNetMode == NET_MODE_WIFI_ONLY);
                bool allow4G   = (currentNetMode == NET_MODE_AUTO || currentNetMode == NET_MODE_4G_ONLY);
                
                // 优先级 A: WiFi (如果允许且已连接)
                if (allowWiFi && MyWiFi.isConnected()) {
                    Serial.println("[Net] Using WiFi.");
                    activeClient = &wifiClient;
                } 
                // 优先级 B: 4G (如果允许)
                else if (allow4G) {
                    if (currentNetMode == NET_MODE_4G_ONLY) {
                        Serial.println("[Net] Force 4G Mode...");
                    } else {
                        Serial.println("[Net] WiFi unavailable. Switching to 4G...");
                    }
                    
                    MyUILogic.updateAssistantStatus("正在使用4G...");
                    
                    // 尝试连接 4G (带超时)
                    if (!My4G.isConnected()) {
                        Serial.println("[Net] 4G Dialing...");
                        if (!My4G.connect(10000L)) { 
                             Serial.println("[Net] 4G Connect Timeout!");
                             MyUILogic.updateAssistantStatus("4G连接失败");
                        }
                    }
                    
                    if (My4G.isConnected()) {
                        Serial.println("[Net] Using 4G Channel.");
                        activeClient = &My4G.getClient();
                    }
                }

                if (activeClient != NULL) {
                    // 执行通信
                    MyServer.chatWithServer(activeClient);
                } else {
                    Serial.println("[Net] Error: No Available Network!");
                    if (currentNetMode == NET_MODE_WIFI_ONLY) MyUILogic.updateAssistantStatus("WiFi未连接");
                    else MyUILogic.updateAssistantStatus("无网络信号");
                    MyUILogic.finishAIState();
                }
            }
            
            if (msg.data != NULL) { 
                free(msg.data);
                msg.data = NULL; 
            }
        }

        // --- 2. 周期性网络维护 ---
        static uint32_t lastCheck = 0;
        if (millis() - lastCheck > 5000) {
            lastCheck = millis();
            // 只有在非 4G 独占模式下，才去维护 WiFi
            if (currentNetMode != NET_MODE_4G_ONLY) {
                if (!MyWiFi.isConnected()) {
                    // MyWiFi.connect 内部有状态检查，不会频繁重连，但最好加个判断
                    MyWiFi.connect("HC-2G", "aa888888");
                }
            }
        }
    }
}

// ================= [Core 0] TaskIR =================
void TaskIR_Code(void *pvParameters) {
    MyIR.init();
    for(;;) {
        MyIR.loop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ================= [Core 0] Task433 =================
void Task433_Code(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    My433.init();
    for(;;) {
        My433.loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ================= Setup =================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n>>> ESP32 Smart Panel Booting... <<<");
    Serial.printf("Server: %s:%d\n", SERVER_HOST, SERVER_PORT);
    
    AudioQueue_Handle = xQueueCreate(5, sizeof(AudioMsg));
    KeyQueue_Handle   = xQueueCreate(10, sizeof(KeyAction));
    IRQueue_Handle    = xQueueCreate(5,  sizeof(IREvent));
    NetQueue_Handle   = xQueueCreate(3, sizeof(NetMessage));

    // 创建任务
    xTaskCreatePinnedToCore(TaskAudio_Code, "Audio",   4096, NULL, 4, &TaskAudio_Handle, 0);
    xTaskCreatePinnedToCore(TaskNet_Code,   "Net",     8192, NULL, 1, &TaskNet_Handle,   0);
    xTaskCreatePinnedToCore(TaskIR_Code,    "IR",      4096, NULL, 1, &TaskIR_Handle,    0);
    xTaskCreatePinnedToCore(Task433_Code,   "RF433",   4096, NULL, 1, &Task433_Handle,   0);
    xTaskCreatePinnedToCore(TaskUI_Code,    "UI",      32768, NULL, 3, &TaskUI_Handle, 1);
    xTaskCreatePinnedToCore(TaskSys_Code,   "Sys",     4096, NULL, 2, &TaskSys_Handle,   1);

    Serial.println(">>> System Ready. <<<");
    Serial.println("Type 'NET=4G' to test 4G only, 'NET=AUTO' to reset.");
}

void loop() {
    vTaskDelete(NULL);
}