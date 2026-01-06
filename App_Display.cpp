#include "App_Display.h"

AppDisplay MyDisplay;

// 定义全局锁变量
SemaphoreHandle_t xGuiSemaphore = NULL;

static const uint16_t screenWidth  = 128;
static const uint16_t screenHeight = 128;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * screenHeight / 10 ];

TFT_eSPI tft = TFT_eSPI(); 

void AppDisplay::my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp); 
}

void AppDisplay::init() {
    // 1. 创建互斥锁
    xGuiSemaphore = xSemaphoreCreateMutex();

    // 2. 硬件初始化//由于是和4G模块共用的线所以智能input模式，缺点是屏幕会闪烁，忽略这个。
    pinMode(PIN_TFT_BL, INPUT);

    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init(); 

    Serial.println("[Display] UI Started with FreeRTOS Mutex.");
}

void AppDisplay::loop() {
    // 尝试获取锁
    // 参数2: 等待时间。这里设为 portMAX_DELAY (死等)，必须拿到锁才能刷新
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        
        // --- 临界区开始 ---
        lv_timer_handler(); 
        // --- 临界区结束 ---
        
        // 归还锁
        xSemaphoreGive(xGuiSemaphore);
    }
}