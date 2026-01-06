#include "App_IR.h"

AppIR MyIR;

const uint16_t kCaptureBufferSize = 1024; 
const uint8_t kTimeout = 15; 

// 引用外部定义的队列 (将在 main.cpp 中定义)
extern QueueHandle_t IRQueue_Handle; 

void AppIR::init() {
    Serial.println("[IR] Initializing...");

    _irRecv = new IRrecv(PIN_IR_RX, kCaptureBufferSize, kTimeout, true);
    _irRecv->enableIRIn(); 

    _irSend = new IRsend(PIN_IR_TX);
    _irSend->begin(); 

    Serial.printf("[IR] RMT Driver Started. RX:%d, TX:%d\n", PIN_IR_RX, PIN_IR_TX);
}

void AppIR::loop() {
    // 检查是否有数据
    if (_irRecv->decode(&_results)) {
        
        // 过滤重复码 (NEC 的 0xFFFFFFFF)
        // 在实际应用中，你可能需要处理重复码来实现 "长按音量键连加" 的功能
        // 这里为了简单，我们先过滤掉
        if (_results.value != 0xFFFFFFFF) {
            
            Serial.printf("[IR] Code: 0x%llX, Bits: %d\n", _results.value, _results.bits);

            // 【关键整合】将收到的指令打包，扔进队列
            if (IRQueue_Handle != NULL) {
                IREvent evt;
                evt.protocol = _results.decode_type;
                evt.value = _results.value;
                evt.bits = _results.bits;
                
                // 发送给主任务处理，不阻塞红外接收
                xQueueSend(IRQueue_Handle, &evt, 0);
            }
        } 
        
        // 准备接收下一条
        _irRecv->resume(); 
    }
}

void AppIR::sendTestSignal() {
    Serial.println("[IR] Sending NEC Signal...");
    
    _irSend->sendNEC(0x12345678, 32);
    
    // RMT 发射后建议稍微延时并重置接收
    // 避免收发状态机错乱
    vTaskDelay(pdMS_TO_TICKS(10)); // 使用 RTOS 延时
    _irRecv->enableIRIn(); 
    
    Serial.println("[IR] Send Done.");
}

void AppIR::sendNEC(uint32_t data) {
    Serial.printf("[IR] Sending NEC: 0x%08X\n", data);
    _irSend->sendNEC(data, 32);
    
    // 发送后稍微延时并恢复接收状态
    vTaskDelay(pdMS_TO_TICKS(20)); 
    _irRecv->enableIRIn(); 
}