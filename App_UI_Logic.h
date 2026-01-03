#ifndef APP_UI_LOGIC_H
#define APP_UI_LOGIC_H

#include <Arduino.h>
#include <lvgl.h>
#include "ui.h"       // SquareLine 导出的 UI 文件
#include "App_Sys.h"  // 按键动作定义 (KEY_SHORT_PRESS 等)
#include <HTTPClient.h>
class AppUILogic {
public:
    void init();
    void loop(); 

    // 处理输入 (被 Task_UI 调用)
    void handleInput(KeyAction action);

    void sendAudioToPC();                    
    void handleAICommand(String jsonString);

private:
    void updateStatusBar();
    void showQRCode();
    void toggleFocus();
    
    void executeLongPressStart();
    void executeLongPressEnd();

    lv_group_t* _uiGroup; 
    lv_obj_t* _qrObj = nullptr; // 用于存放动态生成的二维码对象
    bool _isRecording = false;
};

extern AppUILogic MyUILogic;

#endif