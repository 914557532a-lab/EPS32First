#include "App_Sys.h"

AppSys MySys;

void AppSys::init() {
    // 1. 温度 ADC 初始化
    analogReadResolution(12);       
    analogSetAttenuation(ADC_11db); 
    pinMode(PIN_ADC_TEMP, INPUT);

    // 2. 按键初始化
    pinMode(PIN_KEY2, INPUT_PULLUP);

    Serial.println("[Sys] System Monitor Initialized.");
}

float AppSys::getTemperatureC() {
    int raw = analogRead(PIN_ADC_TEMP);
    // 简单的 NTC 转换公式 (需根据实际电路调整)
    float temp = (4095.0 - raw) / 100.0; 
    return temp; 
}

uint32_t AppSys::getFreeHeap() {
    return ESP.getFreeHeap();
}

// 【这就是之前报错缺失的函数】
// 负责低频的系统监控，比如打印日志、看门狗等
void AppSys::scanLoop() {
    static unsigned long lastLogTime = 0;
    
    // 每 3 秒打印一次系统状态
    if (millis() - lastLogTime > 3000) {
        lastLogTime = millis();
        
        float t = getTemperatureC();
        uint32_t heap = getFreeHeap();
        
        Serial.printf("[Sys] Temp: %.1f C | Heap: %d KB\n", t, heap / 1024);
        
        // 如果温度过高，可以在这里添加报警逻辑
        if (t > 85.0) {
             Serial.println("[Sys] !!! OVERHEAT WARNING !!!");
        }
    }
}

// 按键状态机逻辑 (区分长短按)
KeyAction AppSys::getKeyAction() {
    static bool lastState = HIGH; // INPUT_PULLUP 默认高
    bool currentState = digitalRead(PIN_KEY2);
    KeyAction action = KEY_NONE;

    // 1. 按下瞬间 (Falling Edge)
    if (lastState == HIGH && currentState == LOW) {
        _pressStartTime = millis();
        _isLongPressHandled = false;
    }
    
    // 2. 保持按下中
    if (currentState == LOW) {
        // 计算按住的时间
        if (millis() - _pressStartTime > 800) { // 0.8秒长按阈值
            if (!_isLongPressHandled) {
                action = KEY_LONG_PRESS_START; // 刚达到 0.8s
                _isLongPressHandled = true;    // 标记已处理，防止重复触发 START
            } else {
                action = KEY_LONG_PRESS_HOLD;  // 持续按住
            }
        }
    }

    // 3. 松开瞬间 (Rising Edge)
    if (lastState == LOW && currentState == HIGH) {
        if (_isLongPressHandled) {
            // 如果之前已经是长按状态，现在松开就是长按结束
            action = KEY_LONG_PRESS_END;
        } else {
            // 如果还没触发长按就松开了，那就是短按
            // 简单的消抖：按下时间如果太短(比如小于50ms)可能是抖动，忽略
            if (millis() - _pressStartTime > 50) {
                action = KEY_SHORT_PRESS;
            }
        }
    }

    lastState = currentState;
    return action;
}