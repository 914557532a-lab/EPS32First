/**
 * @file ESP32-Smart-Panel.ino
 * @brief 主程序入口 - FreeRTOS 任务调度器

 * @details 负责初始化所有队列、任务，并将它们分配到双核 CPU 上。
 * * 架构概览:
 * [Core 1] TaskUI  (高优): 负责 LVGL 渲染、UI 逻辑、按键响应 (流畅度关键)
 * [Core 1] TaskSys (中优): 负责 按键扫描、温度监控、系统看门狗
 * [Core 0] TaskAudio(最高): 负责 I2S 音频播放/录音 (实时性关键)
 * [Core 0] TaskNet (低优): 负责 4G 开机、WiFi 连接、网络通信 (慢速 IO)
 * [Core 0] TaskIR  (低优): 负责 红外接收
 * [Core 0] Task433 (低优): 负责 433MHz 射频信号接收
 */

#include <Arduino.h>
#include <FreeRTOS.h>
#include <math.h>
#include <HTTPClient.h>
// --- 引入所有功能模块 ---
#include "Pin_Config.h"
#include "App_Sys.h"
#include "App_Display.h"
#include "App_UI_Logic.h" 
#include "App_Audio.h"
#include "App_WiFi.h"
#include "App_4G.h"
#include "App_IR.h"
#include "App_433.h"
#include "App_Server.h"

// volatile 确保多任务访问时的数据一致性
volatile float g_SystemTemp = 0.0f;

// --- 全局队列句柄 (任务间通信) ---
QueueHandle_t AudioQueue_Handle = NULL; // 发送播放/录音指令
QueueHandle_t KeyQueue_Handle   = NULL; // 发送按键事件 (Sys -> UI)
QueueHandle_t IRQueue_Handle    = NULL; // 发送红外事件 (IR -> Ctrl)
QueueHandle_t NetQueue_Handle = NULL; //网络请求队列

// 如果 433 也需要控制 UI，建议复用 IRQueue 或者新建一个 RFQueue
// 这里暂时假设 433 主要用于后台数据记录，或者通过回调处理

// --- 任务句柄 ---
TaskHandle_t TaskUI_Handle    = NULL;
TaskHandle_t TaskSys_Handle   = NULL;
TaskHandle_t TaskAudio_Handle = NULL;
TaskHandle_t TaskNet_Handle   = NULL;
TaskHandle_t TaskIR_Handle    = NULL;
TaskHandle_t Task433_Handle   = NULL; 

// --- 音频指令结构体 ---
struct AudioMsg {
    uint8_t type; // 0:Beep, 1:StartRec, 2:StopRec
    int param;
};
// =================================================================
// [Core 1] 任务 1: UI 界面 (LVGL 渲染 & 逻辑)
// =================================================================
void TaskUI_Code(void *pvParameters) {
    // 1. 初始化显示和逻辑
    MyDisplay.init();
    MyUILogic.init();

    KeyAction keyMsg;

    for(;;) {
        // 2. 消费按键队列：将 Sys 任务扫描到的按键传递给 UI 逻辑
        // 使用 0 等待时间（非阻塞），以免卡住 UI 刷新
        if (xQueueReceive(KeyQueue_Handle, &keyMsg, 0) == pdTRUE) {
            MyUILogic.handleInput(keyMsg);
        }

        // 3. 刷新 LVGL (内部已包含 xGuiSemaphore 锁)
        MyDisplay.loop();
        
        // 4. 处理 UI 业务逻辑 (如状态栏时间更新)
        MyUILogic.loop();
        
        // 5. 短暂延时，释放 CPU 给同核心的其他任务
        vTaskDelay(pdMS_TO_TICKS(5)); 
    }
}

// ===============================================================

// [Core 1] 任务 2: 系统监视 (按键扫描 & 温度)
// =================================================================
void TaskSys_Code(void *pvParameters) {
    MySys.init();

    static uint32_t lastTempTime = 0;

    for(;;) {
        // 1. 扫描按键
        KeyAction action = MySys.getKeyAction();
        if (action != KEY_NONE) {
            xQueueSend(KeyQueue_Handle, &action, 0);
        }

        // 2. 周期性测温 (每 1000ms 执行一次)
        if (millis() - lastTempTime > 1000) {
            lastTempTime = millis();
            
            // 不再调用本地函数，而是调用模块封装好的函数
            g_SystemTemp = MySys.getTemperatureC(); 
        }

        // 3. 执行系统级扫描 (日志等)
        MySys.scanLoop();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// =================================================================
// [Core 0] 任务 3: 音频处理 (高优先级)
// =================================================================
void TaskAudio_Code(void *pvParameters) {
    MyAudio.init();

    AudioMsg msg;

    for(;;) {
        if (xQueueReceive(AudioQueue_Handle, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.type) {
                case 0:
                    MyAudio.playToneAsync(800, 200);
                    break;
                case 1:
                    MyAudio.startRecording();
                    break;
                case 2:
                    MyAudio.stopRecording();
                    break;
            }
        }
    }
}



// =================================================================
// [Core 0] 任务 4: 网络通信 (WiFi & 4G & HTTP上传)
// =================================================================
void TaskNet_Code(void *pvParameters) {
    // 初始化网络
    vTaskDelay(pdMS_TO_TICKS(1000));
    MyWiFi.init();
    MyWiFi.connect("HC-2G", "aa888888"); // 确保这里也是你的 WiFi 账号密码
    
    // My4G.init(); // 如果不用4G可以注释掉
    
    // 确保服务器IP设置正确 (对应你 Python 电脑的 IP)
    MyServer.init("192.168.1.53", 8080); 

    NetMessage msg;

    for(;;) {
        // 等待 UI 任务发来的信号
        if (xQueueReceive(NetQueue_Handle, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            if (msg.type == NET_EVENT_UPLOAD_AUDIO) {
                Serial.println("[Net] 收到交互请求，开始连接 Python 服务器...");
                
                // 确保 WiFi 已连接
                if (MyWiFi.isConnected()) {
                    // 直接调用 AppServer 的阻塞式处理函数
                    // 因为 TaskNet 优先级低，阻塞这里不会影响 UI 流畅度
                    MyServer.chatWithServer(); 
                } else {
                    Serial.println("[Net] WiFi 未连接，无法上传");
                    // 恢复 UI 状态，否则会一直显示“处理中”
                    MyUILogic.finishAIState();
                }
            }
            
            // 如果 msg.data 不为空（兼容旧代码），记得释放
            if (msg.data != NULL) {
                free(msg.data);
                msg.data = NULL;
            }
        }
        
        // 简单的自动重连机制
        static uint32_t lastCheck = 0;
        if (millis() - lastCheck > 5000) {
            lastCheck = millis();
            if (!MyWiFi.isConnected()) {
                 Serial.println("[Net] WiFi lost, trying to reconnect...");
                 MyWiFi.connect("HC-2G", "aa888888");
            }
        }
    }
}

// =================================================================
// [Core 0] 任务 5: 红外遥控 (IR)
// =================================================================
void TaskIR_Code(void *pvParameters) {
    MyIR.init();
    
    // 引用外部定义的结构体 (在 App_IR.h 中)
    IREvent irEvt;

    for(;;) {
        // 轮询 RMT 接收缓冲区
        MyIR.loop(); 

        // 这里的 loop 内部已经实现了 xQueueSend 到 IRQueue_Handle
        // 所以这里只需要控制轮询频率
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =================================================================
// [Core 0] 任务 6: 433MHz 射频 (RF)
// =================================================================
void Task433_Code(void *pvParameters) {
    // 1. 初始化 433 模块 (SPI 配置 + 芯片复位)
    My433.init();

    for(;;) {
        // 2. 轮询 GPIO 状态或 FIFO
        // App_433.cpp 里的 loop() 只是简单检查一下 GPIO3
        // 如果有信号，My433.loop() 内部会处理并打印日志
        My433.loop();

        // 10ms 轮询间隔，保证遥控器反应灵敏
        // 如果需要极低功耗，可以改用中断触发 (xSemaphoreGiveFromISR)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =================================================================
// 胶水逻辑: 简单的 IR 控制器 (可选)
// =================================================================
// 如果你想让红外遥控也能控制 UI，可以在这里加一个简单的处理函数
// 或者直接把逻辑写在 TaskSys 里。为了简单，这里暂时省略。


// =================================================================
// Arduino Setup
// =================================================================
void setup() {
    Serial.begin(115200);
    delay(2000); // 增加 2 秒延时，等待串口连接建立
    Serial.println("\n\n>>> ESP32 Smart Panel Booting... <<<");

    MyWiFi.init();
    MyWiFi.connect("HC-2G", "aa888888");
    MyServer.init("192.168.1.53", 8080);
    // 1. 创建队列
    AudioQueue_Handle = xQueueCreate(5, sizeof(AudioMsg));
    KeyQueue_Handle   = xQueueCreate(10, sizeof(KeyAction));
    IRQueue_Handle    = xQueueCreate(5,  sizeof(IREvent)); 
    NetQueue_Handle   = xQueueCreate(3, sizeof(NetMessage));

    if (!AudioQueue_Handle || !KeyQueue_Handle || !IRQueue_Handle) {
        Serial.println("Error: Queue creation failed!");
        while(1);
    }

    // 2. 创建任务 (分配核心)
    // -----------------------------------------------------------------------
    // 任务函数       任务名      栈大小   参数  优先级  句柄          核心
    // -----------------------------------------------------------------------
    
    // [Core 0] 协议/驱动层 (负责处理外设通讯)
    // 音频优先级必须最高 (4)，否则 WiFi 传输时声音会破音
    xTaskCreatePinnedToCore(TaskAudio_Code, "Audio",   4096, NULL, 4, &TaskAudio_Handle, 0);
    // 网络任务栈要大 (8192)，WiFi 库吃内存
    xTaskCreatePinnedToCore(TaskNet_Code,   "Net",     8192, NULL, 1, &TaskNet_Handle,   0);
    xTaskCreatePinnedToCore(TaskIR_Code,    "IR",      4096, NULL, 1, &TaskIR_Handle,    0);
    // 433 任务，低优先级轮询
    xTaskCreatePinnedToCore(Task433_Code,   "433",     4096, NULL, 1, &Task433_Handle,   0);

    // [Core 1] 应用/交互层 (负责业务逻辑和界面)
    // UI 优先级设为 3，保证交互丝滑
    xTaskCreatePinnedToCore(TaskUI_Code, "UI", 32768, NULL, 3, &TaskUI_Handle, 1);
    // Sys 负责按键，优先级 2 即可
    xTaskCreatePinnedToCore(TaskSys_Code,   "Sys",     4096, NULL, 2, &TaskSys_Handle,   1);

    Serial.println(">>> All Tasks Started. System Ready. <<<");
}

// 在 loop() 或 任务中
void loop() {
    vTaskDelete(NULL);
}