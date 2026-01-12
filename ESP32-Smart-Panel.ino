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
// [新增] 包含 433 头文件
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
void TaskNet_Code(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    MyWiFi.init();
    MyWiFi.connect("HC-2G", "aa888888"); 
    MyServer.init("192.168.1.53", 8080);
    NetMessage msg;

    for(;;) {
        if (xQueueReceive(NetQueue_Handle, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (msg.type == NET_EVENT_UPLOAD_AUDIO) {
                Serial.println("[Net] Upload Request...");
                if (MyWiFi.isConnected()) {
                    MyServer.chatWithServer();
                } else {
                    Serial.println("[Net] WiFi Disconnected.");
                    MyUILogic.finishAIState();
                }
            }
            if (msg.data != NULL) { free(msg.data); msg.data = NULL; }
        }
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