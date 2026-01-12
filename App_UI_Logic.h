#ifndef APP_UI_LOGIC_H
#define APP_UI_LOGIC_H

#include <Arduino.h>
#include <lvgl.h>
#include "ui.h"       // SquareLine 导出的 UI 文件
#include "App_Sys.h"  // 按键动作定义
#include <HTTPClient.h>

class AppUILogic {
public:
    void init();
    void loop(); 

    // 处理输入 (被 Task_UI 调用)
    void handleInput(KeyAction action);
    
    // --- 新增/修复的核心函数 ---
    void finishAIState();                         // 结束 AI 状态，恢复 UI
    void updateAssistantStatus(const char* status); // 更新状态栏或提示文字
    void showReplyText(const char* text);         // 显示 AI 回复的内容
    
    void sendAudioToPC();                    
    void handleAICommand(String jsonString);

private:
    void updateStatusBar();
    void showQRCode();
    void toggleFocus();
    
    void executeLongPressStart();
    void executeLongPressEnd();

    lv_group_t* _uiGroup; 
    lv_obj_t* _qrObj = nullptr; 
    bool _isRecording = false;
};

extern AppUILogic MyUILogic;

#endif