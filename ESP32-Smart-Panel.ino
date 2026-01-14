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
// [修改] 声明任务句柄
TaskHandle_t Task433_Handle   = NULL;

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
    // 1. 初始化 WiFi
    MyWiFi.init();
    MyWiFi.connect("HC-2G", "aa888888");
    
    // 2. 初始化 4G
    My4G.init();
    My4G.powerOn(); // 开机
    
    MyServer.init("192.168.1.53", 8080);
    
    WiFiClient wifiClient; 
    NetMessage msg;

    for(;;) {
        // --- [新增] 串口 AT 指令监听 ---
        // 允许用户在串口监视器输入 "AT+..." 进行测试
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');
            input.trim();
            if (input.length() > 0) {
                // 如果是 AT 开头的指令，或者用户想强制发其他指令
                My4G.sendRawAT(input);
            }
        }

        // --- 1. 处理消息队列 ---
        // 将等待时间设为 20ms，保证能快速回到上方响应串口 AT 指令
        if (xQueueReceive(NetQueue_Handle, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            
            if (msg.type == NET_EVENT_UPLOAD_AUDIO) {
                Serial.println("[Net] Upload Request Received.");
                Client* activeClient = NULL;
                
                // 优先级 A: WiFi
                if (MyWiFi.isConnected()) {
                    Serial.println("[Net] Using WiFi.");
                    activeClient = &wifiClient;
                } 
                // 优先级 B: 4G
                else {
                    Serial.println("[Net] WiFi lost! Switching to 4G...");
                    MyUILogic.updateAssistantStatus("正在切换4G...");
                    
                    // 如果 4G 未连接，尝试连接
                    if (!My4G.isConnected()) {
                        Serial.println("[Net] 4G dialing...");
                        
                        // [修改] 使用 10秒 超时，避免长时间卡死
                        // 如果 10秒 连不上，本次请求失败，但不会导致 UI 死机
                        if (My4G.connect(10000L)) { 
                             // 连接成功
                        } else {
                             Serial.println("[Net] 4G Connect Timeout!");
                             MyUILogic.updateAssistantStatus("网络连接失败");
                        }
                    }
                    
                    if (My4G.isConnected()) {
                        Serial.println("[Net] Using 4G.");
                        activeClient = &My4G.getClient();
                    }
                }

                if (activeClient != NULL && activeClient->connected() == false) {
                     // 再次确认连接状态，防止空指针
                     if(!activeClient->connect("192.168.1.53", 8080)) { 
                         // 这里只是示例，实际上 TinyGSM Client 连接在 connect() 里已经处理
                     }
                }

                if (activeClient != NULL) {
                    MyServer.chatWithServer(activeClient);
                } else {
                    Serial.println("[Net] Error: No Network!");
                    MyUILogic.updateAssistantStatus("无网络连接");
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
            if (!MyWiFi.isConnected()) {
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

// ================= [Core 0] Task433 [新增] =================
void Task433_Code(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待其他硬件就绪
    My433.init();
    for(;;) {
        My433.loop();
        // 轮询间隔，给 Core 0 其他任务留时间
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ================= Setup =================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n>>> ESP32 Smart Panel Booting... <<<");
    
    AudioQueue_Handle = xQueueCreate(5, sizeof(AudioMsg));
    KeyQueue_Handle   = xQueueCreate(10, sizeof(KeyAction));
    IRQueue_Handle    = xQueueCreate(5,  sizeof(IREvent)); 
    NetQueue_Handle   = xQueueCreate(3, sizeof(NetMessage));

    // 创建任务
    xTaskCreatePinnedToCore(TaskAudio_Code, "Audio",   4096, NULL, 4, &TaskAudio_Handle, 0);
    xTaskCreatePinnedToCore(TaskNet_Code,   "Net",     8192, NULL, 1, &TaskNet_Handle,   0);
    xTaskCreatePinnedToCore(TaskIR_Code,    "IR",      4096, NULL, 1, &TaskIR_Handle,    0);
    
    // [新增] 启动 433 任务
    xTaskCreatePinnedToCore(Task433_Code,   "RF433",   4096, NULL, 1, &Task433_Handle,   0);

    xTaskCreatePinnedToCore(TaskUI_Code,    "UI",      32768, NULL, 3, &TaskUI_Handle, 1);
    xTaskCreatePinnedToCore(TaskSys_Code,   "Sys",     4096, NULL, 2, &TaskSys_Handle,   1);

    Serial.println(">>> System Ready. <<<");
}

void loop() {
    vTaskDelete(NULL);
}