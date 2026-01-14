/**
 * @file App_UI_Logic.h
 */
#ifndef APP_UI_LOGIC_H
#define APP_UI_LOGIC_H

#include <Arduino.h>
#include <lvgl.h>
#include "ui.h"

// [新增] 必须包含这个头文件，因为 KeyAction 是在这里定义的
// 如果没有这一行，编译器就不认识 KeyAction
#include "App_Sys.h" 

class AppUILogic {
public:
    void init();
    void loop();
    
    // 处理按键输入 (KeyAction 类型依赖 App_Sys.h)
    void handleInput(KeyAction action);
    
    // 处理来自 AI 的指令
    void handleAICommand(String jsonString);

    // 更新状态栏文字
    void updateAssistantStatus(const char* status);
    
    // 显示 AI 回复文字
    void showReplyText(const char* text);
    
    // 完成 AI 流程（恢复 UI 状态）
    void finishAIState();
    
    // 被动设置信号强度
    void setSignalCSQ(int csq);

private:
    void updateStatusBar();
    void toggleFocus();
    void executeLongPressStart();
    void executeLongPressEnd();
    void sendAudioToPC();
    void showQRCode();

    lv_group_t* _uiGroup;
    lv_obj_t* _qrObj = NULL;
    bool _isRecording = false;
    
    // 缓存的信号值
    int _cachedCSQ = 0;
};

extern AppUILogic MyUILogic;

#endif