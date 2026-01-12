#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <Arduino.h>
#include <TFT_eSPI.h> 
#include <lvgl.h>      
#include "ui.h"         
#include "Pin_Config.h" 

// 引入 FreeRTOS
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h> 

class AppDisplay {
public:
    void init();
    void loop(); 
    
    // [新增] 切换背光函数
    void toggleBacklight();

private:
    static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
    
    // [新增] 记录背光状态，默认是开
    bool _backlightState = true; 
};

extern AppDisplay MyDisplay;
extern SemaphoreHandle_t xGuiSemaphore;

#endif