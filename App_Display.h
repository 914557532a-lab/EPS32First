#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <Arduino.h>
#include <TFT_eSPI.h> 
#include <lvgl.h>      
#include "ui.h"         
#include "Pin_Config.h" 

// 引入 FreeRTOS
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h> // 引入信号量/互斥锁

class AppDisplay {
public:
    void init();
    void loop(); // 放入 TaskUI 的循环中

private:
    static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
};

extern AppDisplay MyDisplay;

// 【新增】声明一个全局互斥锁，供所有任务使用
extern SemaphoreHandle_t xGuiSemaphore;

#endif