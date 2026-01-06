#include "App_Sys.h"
#include <math.h> 
#include "Pin_Config.h"
AppSys MySys;

// --- [搬运] NTC 热敏电阻参数 ---
// 放在 cpp 文件顶部作为局部常量即可，不需要暴露给外部
const float R7_VAL  = 8200.0;    // 上拉电阻 8.2K
const float R48_VAL = 200000.0;  // 下拉电阻 200K
const float VCC     = 3300.0;    // 系统电压 3.3V
const float NTC_R0  = 100000.0;  // 25°C 阻值 100K
const float NTC_B   = 3950.0;    // B值
const float NTC_T0  = 298.15;    // 25°C 的开尔文温度
extern volatile float g_SystemTemp;
void AppSys::init() {
    // 1. 温度 ADC 初始化
    analogReadResolution(12);       
    analogSetAttenuation(ADC_11db); 
    pinMode(PIN_ADC_TEMP, INPUT);

    // 2. 按键初始化
    // 注意：请确保 Pin_Config.h 里定义了 PIN_KEY2，如果没有请检查头文件引用
    #ifdef PIN_KEY2
    pinMode(PIN_KEY2, INPUT_PULLUP);
    #endif

    Serial.println("[Sys] System Monitor Initialized.");
}

// [修改] 使用 Steinhart-Hart 算法的高精度测温
float AppSys::getTemperatureC() {
    // 1. 读取毫伏值 (使用 ESP32 工厂校准 API，比 analogRead 更准)
    int raw_mv = analogReadMilliVolts(PIN_ADC_TEMP);

    // 2. 异常过滤 (开路/短路保护)
    if (raw_mv < 100 || raw_mv > 3200) return -99.0; // 返回一个错误值

    // 3. 计算阻值 (电路结构: 3V3 -> R7 -> NTC -> [ADC] -> R48 -> GND)
    // 公式: R_total = (VCC * R48) / V_adc
    float r_total = (VCC * R48_VAL) / (float)raw_mv;
    
    // R_ntc = R_total - R7 - R48
    float r_ntc = r_total - R7_VAL - R48_VAL;

    // 4. 计算温度 (Steinhart-Hart)
    if (r_ntc > 0) {
        float ln_ratio = log(r_ntc / NTC_R0);
        float kelvin = 1.0 / ( (1.0 / NTC_T0) + (ln_ratio / NTC_B) );
        return kelvin - 273.15; // 转为摄氏度
    }
    
    return 0.0;
}

uint32_t AppSys::getFreeHeap() {
    return ESP.getFreeHeap();
}

void AppSys::scanLoop() {
    static unsigned long lastLogTime = 0;
    
    if (millis() - lastLogTime > 3000) {
        lastLogTime = millis();
        
        // 这里直接调用刚刚升级过的 getTemperatureC
        float t = getTemperatureC();
        uint32_t heap = getFreeHeap();
        g_SystemTemp = t;
        Serial.printf("[Sys] Temp: %.1f C | Heap: %d KB\n", t, heap / 1024);
        
        if (t > 85.0) {
             Serial.println("[Sys] !!! OVERHEAT WARNING !!!");
        }
    }
}


KeyAction AppSys::getKeyAction() {
    static bool lastState = HIGH; 
    bool currentState = digitalRead(PIN_KEY2);
    KeyAction action = KEY_NONE;
    // ...
    if (lastState == HIGH && currentState == LOW) {
        _pressStartTime = millis();
        _isLongPressHandled = false;
    }
    if (currentState == LOW) {
        if (millis() - _pressStartTime > 800) { 
            if (!_isLongPressHandled) {
                action = KEY_LONG_PRESS_START; 
                _isLongPressHandled = true;   
            } else {
                action = KEY_LONG_PRESS_HOLD; 
            }
        }
    }
    if (lastState == LOW && currentState == HIGH) {
        if (_isLongPressHandled) {
            action = KEY_LONG_PRESS_END;
        } else {
            if (millis() - _pressStartTime > 50) {
                action = KEY_SHORT_PRESS;
            }
        }
    }
    lastState = currentState;
    return action;
}