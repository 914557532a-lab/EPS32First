#include "App_UI_Logic.h"
#include "App_Display.h" 
#include "App_4G.h"      
#include "App_Audio.h"   
#include "App_WiFi.h"    
#include <time.h> 
#include "App_433.h"
#include <ArduinoJson.h> 
#include "App_Sys.h"

AppUILogic MyUILogic;

extern volatile float g_SystemTemp; 

// --- 修复点 1: 添加 handleAICommand 的实现 (或者在头文件中声明它) ---
void AppUILogic::handleAICommand(String jsonString) {
    // 1. 解析 JSON
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, jsonString);

    if (error) {
        Serial.print("JSON 解析失败: ");
        Serial.println(error.c_str());
        return;
    }

    const char* object = doc["object"]; // "AirConditioner", "CarDoor"
    const char* action = doc["action"]; // "Open", "SetTemp"
    int value = doc["value"];           // 24

    Serial.printf("[AI] 执行指令: 对象=%s, 动作=%s, 值=%d\n", object, action, value);

    // 2. 根据指令执行动作
    if (strcmp(object, "CarDoor") == 0) {
        if (strcmp(action, "Open") == 0) {
            // 调用 433 发送开锁信号
            // 假设 My433 有一个 send 函数，或者你在这里直接控制 GPIO
            // My433.send(0x123456, 24); 
            Serial.println(">>> 正在通过 433MHz 打开车门...");
        }
    } 
    else if (strcmp(object, "AirConditioner") == 0) {
        // 这里可能需要发送红外或者其他逻辑
        Serial.printf(">>> 设置空调温度为: %d\n", value);
    }
}

void AppUILogic::init() {
    _uiGroup = lv_group_create();
    
    lv_group_add_obj(_uiGroup, ui_ButtonAI);
    lv_group_add_obj(_uiGroup, ui_ButtonLink);
    
    lv_group_focus_obj(ui_ButtonAI);

    // 建议在 WiFi/4G 连接成功后再通过事件触发 configTime，这里先预设
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    
    Serial.println("[UI Logic] Init Done.");
}

void AppUILogic::toggleFocus() {
    lv_group_focus_next(_uiGroup);
    MyAudio.playToneAsync(600, 50); 
}

void AppUILogic::showQRCode() {
    String content = "Unknown";
    content = "IMEI:865432123456789"; // 测试数据

    const char* data = content.c_str();
    Serial.printf("[UI] Showing QR: %s\n", data);

    if (_qrObj == NULL) {
        _qrObj = lv_qrcode_create(ui_PanelQR, 110, lv_color_black(), lv_color_white());
        lv_obj_center(_qrObj);
        if(ui_ImageQR) lv_obj_add_flag(ui_ImageQR, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(_qrObj, 4, 0);
        lv_obj_set_style_border_color(_qrObj, lv_color_white(), 0);
    }
    lv_qrcode_update(_qrObj, data, strlen(data));
}

void AppUILogic::executeLongPressStart() {
    lv_obj_t* focusedObj = lv_group_get_focused(_uiGroup);

    if (focusedObj == ui_ButtonAI) {
        Serial.println("[UI] LongPress: Start Recording");
        
        // --- 视觉交互修改 START ---
        // 隐藏常规组件
        if(ui_ButtonAI) lv_obj_add_flag(ui_ButtonAI, LV_OBJ_FLAG_HIDDEN);
        if(ui_ButtonLink) lv_obj_add_flag(ui_ButtonLink, LV_OBJ_FLAG_HIDDEN);
        

        MyAudio.startRecording();
        _isRecording = true;

    } else if (focusedObj == ui_ButtonLink) {
        Serial.println("[UI] LongPress: Go to QR");
        MyAudio.playToneAsync(1000, 100);
        _ui_screen_change(&ui_QRScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, &ui_QRScreen_screen_init);
        showQRCode();
    }
}

void AppUILogic::sendAudioToPC() {
    // 1. 检查数据有效性
    if (MyAudio.record_buffer == NULL || MyAudio.record_data_len == 0) {
        Serial.println("[UI] 无录音数据，取消上传");
        return;
    }

    // 2. 组装消息 (不拷贝数据，只发送事件类型)
    NetMessage msg;
    msg.type = NET_EVENT_UPLOAD_AUDIO; // 复用这个枚举，但含义变为 "Start TCP Chat"
    msg.len  = 0; 
    msg.data = NULL; // 不需要拷贝数据，AppServer 直接读取 MyAudio 全局缓冲区

    // 3. 发送给网络任务
    if (xQueueSend(NetQueue_Handle, &msg, 0) == pdTRUE) {
        Serial.println("[UI] 已通知网络任务开始处理录音");
    } else {
        Serial.println("[UI] 错误：网络队列已满");
    }
}

// 3. 修改长按结束：更新文本为“处理中”，但不恢复 UI
void AppUILogic::executeLongPressEnd() {
    if (_isRecording) {
        Serial.println("[UI] Released: Stop Recording");
        MyAudio.stopRecording();
        _isRecording = false;
        
        sendAudioToPC(); 
    }
}
void AppUILogic::finishAIState() {
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        Serial.println("[UI] AI Process Finished. Restoring UI.");

        // 恢复按钮显示
        if(ui_ButtonAI) {
            lv_obj_clear_flag(ui_ButtonAI, LV_OBJ_FLAG_HIDDEN);
            // 确保颜色改回默认（防止上次长按变红没改回来）
            lv_obj_set_style_bg_color(ui_ButtonAI, lv_color_hex(0xF9F9F9), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        if(ui_ButtonLink) lv_obj_clear_flag(ui_ButtonLink, LV_OBJ_FLAG_HIDDEN);

        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::updateStatusBar() {
    // 必须检查互斥锁，防止与 Core 0 的 App_Server 冲突
    if (xSemaphoreTake(xGuiSemaphore, 0) == pdTRUE) {
        
        // 1. 确保当前确实在主屏幕，且主屏幕对象有效
        // 如果当前是二维码屏幕，ui_MainScreen 可能已被标记为无效或不可见
        if (lv_scr_act() == ui_MainScreen && ui_MainScreen != NULL) {
            
            // 2. 信号栏刷新 (假设 ui_Bar4gsignal 也是主屏幕的子对象)
            if (ui_Bar4gsignal) {
                 int signalLevel = 75; 
                 lv_bar_set_value(ui_Bar4gsignal, signalLevel, LV_ANIM_ON);
            }

            // 3. 时间刷新
            if (ui_LabelTime) {
                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 0)) { 
                    char timeStr[10];
                    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    lv_label_set_text(ui_LabelTime, timeStr);
                }
            }

            // 4. 温度刷新 (最可能的崩溃点)
            // 务必确保 ui_LabelDebug 不为空
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::handleInput(KeyAction action) {
    if (action == KEY_NONE) return;

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        
        lv_obj_t* currentScreen = lv_scr_act();

        switch (action) {
            case KEY_SHORT_PRESS:
                if (currentScreen == ui_MainScreen) {
                    toggleFocus();
                } 
                else if (currentScreen == ui_QRScreen) {
                    _ui_screen_change(&ui_MainScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, &ui_MainScreen_screen_init);
                    if (_qrObj != NULL) {
                        lv_obj_del(_qrObj);
                        _qrObj = NULL;
                    }
                }
                break;

            case KEY_LONG_PRESS_START:
                if (currentScreen == ui_MainScreen) {
                    executeLongPressStart();
                }
                break;

            case KEY_LONG_PRESS_END:
                if (currentScreen == ui_MainScreen) {
                    executeLongPressEnd();
                }
                break;
                
            default: break;
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppUILogic::loop() {
    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate > 1000) {
        lastUpdate = millis();
        if (lv_scr_act() == ui_MainScreen) {
            updateStatusBar();
        }
    }
}