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
#include <math.h> // [新增] 用于 log() 函数计算温度
#include <HTTPClient.h>
// --- 引入所有功能模块 ---
#include "Pin_Config.h"
#include "App_Sys.h"
#include "App_Display.h"
#include "App_UI_Logic.h" // UI业务逻辑层
#include "App_Audio.h"
#include "App_WiFi.h"
#include "App_4G.h"
#include "App_Flash.h"
#include "App_IR.h"     // 引入 433红外线模块头文件
#include "App_433.h"    // 引入 433 模块头文件

// 电阻参数 (单位: 欧姆)
const float R7_VAL  = 8200.0;    // 上拉电阻 8.2K
const float R48_VAL = 200000.0;  // 下拉电阻 200K
const float VCC     = 3300.0;    // 系统电压 (mV), 3.3V

// NTC 热敏电阻参数 (HNTC0603-104F3950FB)
const float NTC_R0  = 100000.0;  // 25°C时的标称阻值 (100K)
const float NTC_B   = 3950.0;    // B值
const float NTC_T0  = 298.15;    // 25°C 对应的开尔文温度 (273.15 + 25)

// [新增] 全局温度变量 (UI任务读取，Sys任务写入)
// volatile 确保多任务访问时的数据一致性
volatile float g_SystemTemp = 0.0f;

// --- 全局队列句柄 (任务间通信) ---
QueueHandle_t AudioQueue_Handle = NULL; // 发送播放/录音指令
QueueHandle_t KeyQueue_Handle   = NULL; // 发送按键事件 (Sys -> UI)
QueueHandle_t IRQueue_Handle    = NULL; // 发送红外事件 (IR -> Ctrl)

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
// [新增] 辅助逻辑: 温度计算 (供 TaskSys 调用)
// =================================================================
void updateTemperature() {
    // 1. 读取毫伏值 (依赖 ESP32 工厂校准)
    int raw_mv = analogReadMilliVolts(PIN_ADC_TEMP);

    // 2. 异常过滤 (开路/短路保护)
    if (raw_mv < 100 || raw_mv > 3200) return;

    // 3. 计算阻值 (电路结构: 3V3 -> R7 -> NTC -> [ADC] -> R48 -> GND)
    // 公式推导: R_total = (VCC * R48) / V_adc
    float r_total = (VCC * R48_VAL) / (float)raw_mv;
    
    // R_ntc = R_total - R7 - R48
    float r_ntc = r_total - R7_VAL - R48_VAL;

    // 4. 计算温度 (Steinhart-Hart)
    if (r_ntc > 0) {
        float ln_ratio = log(r_ntc / NTC_R0);
        float kelvin = 1.0 / ( (1.0 / NTC_T0) + (ln_ratio / NTC_B) );
        
        // 更新全局变量
        g_SystemTemp = kelvin - 273.15;
    }
}

// =================================================================
// [Core 1] 任务 1: UI 渲染与交互
// =================================================================
void TaskUI_Code(void *pvParameters) {
    // 1. 初始化底层显示 (TFT + LVGL)
    MyDisplay.init();
    
    // 2. 初始化 UI 业务逻辑 (Group, Focus, Events)
    MyUILogic.init();

    KeyAction keyEvent;

    for(;;) {
        // --- A. 渲染核心 (带锁) ---
        MyDisplay.loop(); 

        // --- B. 处理按键输入 ---
        // 非阻塞接收：如果有按键按下，Sys 任务会塞入队列
        if (xQueueReceive(KeyQueue_Handle, &keyEvent, 0) == pdTRUE) {
            MyUILogic.handleInput(keyEvent);
        }

        // --- C. 周期性 UI 逻辑 ---
        // 比如更新时间、刷新 4G 信号条动画
        // [提示] 在这里可以通过读取 g_SystemTemp 来显示温度
        MyUILogic.loop();

        // 保持约 200FPS 的刷新率，给 CPU 喘息
        vTaskDelay(pdMS_TO_TICKS(5)); 
    }
}

// =================================================================
// [Core 1] 任务 2: 系统监视 (按键扫描 & 温度)
// =================================================================
void TaskSys_Code(void *pvParameters) {
    MySys.init();

    // [新增] ADC 初始化配置
    analogReadResolution(12);       // 12位分辨率
    analogSetAttenuation(ADC_11db); // 必须设置 11dB 衰减，以支持 >1.1V 的电压测量

    // [新增] 静态时间戳，用于控制测温频率
    static uint32_t lastTempTime = 0;

    for(;;) {
        // 1. 扫描按键状态机 (短按/长按判断)
        // 这个函数内部维护了状态，调用间隔决定了去抖效果
        KeyAction action = MySys.getKeyAction();

        // 2. 如果有有效动作，发送给 UI 任务
        if (action != KEY_NONE) {
            // 发送消息，如果队列满了就丢弃 (防止卡死)
            xQueueSend(KeyQueue_Handle, &action, 0);
        }

        // 3. [新增] 周期性测温 (每 1000ms 执行一次)
        // 使用非阻塞方式，不影响按键扫描
        if (millis() - lastTempTime > 1000) {
            lastTempTime = millis();
            updateTemperature(); // 更新全局变量 g_SystemTemp
        }

        // 4. 执行系统级扫描 (其他后台任务)
        MySys.scanLoop();

        // 20ms 周期 = 50Hz 采样率，对人类按键来说足够灵敏且稳定
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
// [Core 0] 任务 4: 网络通信 (WiFi & 4G)
// =================================================================
void TaskNet_Code(void *pvParameters) {
    // 1. 初始化 Flash (SPI 总线，注意防冲突)
    MyFlash.init();

    // 2. 初始化 WiFi (非阻塞)
    MyWiFi.init();
    // 连接 WiFi (可以把 SSID 存在 Flash 里读出来)
    MyWiFi.connect("HC-5G", "aa888888");

    // 3. 初始化 4G (这是一个耗时 8 秒的操作！)
    // 放在这里完全不会卡住 UI
    My4G.init(); 
    My4G.powerOn(); // 开机 + 握手 + 获取 IMEI

    for(;;) {
        // --- 周期性网络维护 ---
        
        // 比如：每 5 秒检查一次 4G 信号强度，更新给 UI 逻辑层
        // 注意：这里不要直接调 lvgl 函数，而是更新全局变量或发消息
        // 为了简单，我们假设 AppUILogic 会自己去查 My4G.getRSSI()
        
        // 也可以在这里处理 WiFi 重连逻辑
        
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    Serial.println("\n\n>>> ESP32 Smart Panel Booting... <<<");

    // 1. 创建队列
    AudioQueue_Handle = xQueueCreate(5, sizeof(AudioMsg));
    KeyQueue_Handle   = xQueueCreate(10, sizeof(KeyAction));
    IRQueue_Handle    = xQueueCreate(5,  sizeof(IREvent)); 

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
    xTaskCreatePinnedToCore(TaskUI_Code,    "UI",      8192, NULL, 3, &TaskUI_Handle,    1);
    // Sys 负责按键，优先级 2 即可
    xTaskCreatePinnedToCore(TaskSys_Code,   "Sys",     4096, NULL, 2, &TaskSys_Handle,   1);

    Serial.println(">>> All Tasks Started. System Ready. <<<");
}

void loop() {
    // 主循环必须留空，或直接删除任务以释放 loop 占用的少量 RAM
    vTaskDelete(NULL); 
}