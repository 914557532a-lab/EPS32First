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

// [新增] 切换背光实现
void AppDisplay::toggleBacklight() {
    // _backlightState = !_backlightState;
    // digitalWrite(PIN_TFT_BL, _backlightState ? HIGH : LOW);
    Serial.println("[Display] Backlight control DISABLED for 4G NetLight test.");
}

void AppDisplay::init() {
    gpio_reset_pin((gpio_num_t)3); 
    pinMode(3, OUTPUT); 
    
    xGuiSemaphore = xSemaphoreCreateMutex();

    // [修改 2] 注释掉背光引脚初始化
    // 理由：GPIO14 现在连接着 4G 模块的 NET_LIGHT 引脚。
    // 我们不能将其设为 OUTPUT，否则会干扰 4G 模块输出的状态信号。
    // pinMode(PIN_TFT_BL, OUTPUT);
    // digitalWrite(PIN_TFT_BL, HIGH); // 默认开启
    // _backlightState = true;
    
    // [可选] 显式将其设为输入，确保安全 (虽然 App_4G 里已经设了，多设一次无害)
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
    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        lv_timer_handler(); 
        xSemaphoreGive(xGuiSemaphore);
    }
}