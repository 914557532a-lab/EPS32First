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
#define SERVER_HOST  "35.tcp.cpolar.top"
#define SERVER_PORT  10985 

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

// ================= [Core 0] TaskNet (修复后) =================
void TaskNet_Code(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 1. 初始化
    MyWiFi.init();
    if (currentNetMode != NET_MODE_4G_ONLY) {
        MyWiFi.connect("HC-2G", "aa888888");
    } else {
        WiFi.mode(WIFI_OFF);
    }
    
    My4G.init();
    My4G.powerOn();
    MyServer.init(SERVER_HOST, SERVER_PORT);
    
    WiFiClient wifiClient; 
    NetMessage msg;
    
    static uint32_t lastSignalCheck = 0;

    for(;;) {
        // --- [修复] 串口指令监听：增加模式切换逻辑 ---
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            
            if (input == "NET=4G") {
                currentNetMode = NET_MODE_4G_ONLY;
                Serial.println("\n>>> 切换模式: 强制 4G (WiFi 关闭)");
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            } 
            else if (input == "NET=AUTO") {
                currentNetMode = NET_MODE_AUTO;
                Serial.println("\n>>> 切换模式: 自动 (WiFi 优先)");
                MyWiFi.connect("HC-2G", "aa888888");
            }
            else if (input.length() > 0) {
                // 不是切换指令，才当作 AT 指令发送
                My4G.sendRawAT(input); 
            }
        }

        // --- 1. 处理消息队列 ---
        if (xQueueReceive(NetQueue_Handle, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (msg.type == NET_EVENT_UPLOAD_AUDIO) {
                Serial.println("[Net] Upload Request Received.");
                Client* activeClient = NULL;
                
                // 网络选择逻辑
                if (currentNetMode != NET_MODE_4G_ONLY && MyWiFi.isConnected()) {
                    Serial.println("[Net] Using WiFi.");
                    activeClient = &wifiClient;
                } else {
                    Serial.println("[Net] Using 4G...");
                    MyUILogic.updateAssistantStatus("正在使用4G...");
                    
                    // 尝试连接
                    if (My4G.connect(15000L)) {
                         activeClient = &My4G.getClient();
                    } else {
                         MyUILogic.updateAssistantStatus("4G连接失败");
                    }
                }

                if (activeClient) {
                    MyServer.chatWithServer(activeClient);
                } else {
                    MyUILogic.updateAssistantStatus("无网络");
                    MyUILogic.finishAIState();
                }
            }
            if (msg.data) { free(msg.data); msg.data = NULL; }
        }

        // --- 2. [修复] 信号查询逻辑 ---
        // 只要 4G 模块初始化了，就应该允许查询信号，不一定要等到完全 Connected
        if (millis() - lastSignalCheck > 2000) {
            lastSignalCheck = millis();
            
            // 只要不是纯 WiFi 模式，或者是强制 4G 模式，就读信号
            // 这里的逻辑：只要模块开着，就去读，防止 UI 没数据
            int csq = My4G.getSignalCSQ();
            // 过滤一下无效值 99，如果读到 99 就不更新 UI，或者显示 0
            if (csq == 99) csq = 0; 
            MyUILogic.setSignalCSQ(csq);
        }
        
        // --- 3. WiFi 维护 ---
        static uint32_t lastWiFiCheck = 0;
        if (millis() - lastWiFiCheck > 5000) {
            lastWiFiCheck = millis();
            if (currentNetMode != NET_MODE_4G_ONLY && !MyWiFi.isConnected()) {
                MyWiFi.connect("HC-2G", "aa888888");
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
    // [修改] 注释掉 433 任务，防止日志干扰
    // xTaskCreatePinnedToCore(Task433_Code,   "RF433",   4096, NULL, 1, &Task433_Handle,   0);
    
    xTaskCreatePinnedToCore(TaskUI_Code,    "UI",      32768, NULL, 3, &TaskUI_Handle, 1);
    xTaskCreatePinnedToCore(TaskSys_Code,   "Sys",     4096, NULL, 2, &TaskSys_Handle,   1);

    Serial.println(">>> System Ready. <<<");
    Serial.println("Type 'NET=4G' to test 4G only, 'NET=AUTO' to reset.");
}

void loop() {
    vTaskDelete(NULL);
}