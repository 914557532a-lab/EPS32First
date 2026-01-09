#include "App_IR.h"

AppIR MyIR;

const uint16_t kCaptureBufferSize = 1024; 
const uint8_t kTimeout = 20; 

extern QueueHandle_t IRQueue_Handle; 

// 1. 全局只保留 ac 对象，它会独占管理 PIN_IR_TX
IRElectraAc ac(PIN_IR_TX); 

void AppIR::init() {
    Serial.println("[IR] Initializing...");

    // --- 接收部分 (不变) ---
    _irRecv = new IRrecv(PIN_IR_RX, kCaptureBufferSize, kTimeout, true);
    _irRecv->enableIRIn(); 

    // --- 发送部分 (关键修改) ---
    // [删除] 不要在这里 new IRsend，会和 ac 对象冲突！
    // _irSend = new IRsend(PIN_IR_TX);
    // _irSend->begin(); 

    // 初始化空调对象 (它会自动初始化 RMT 驱动)
    Serial.println("IR: Initializing Electra AC...");
    ac.begin(); 
    
    // 初始化默认状态
    ac.off();
    ac.setFan(kElectraAcFanAuto);
    ac.setMode(kElectraAcCool);
    ac.setTemp(26);
    ac.setSwingV(kElectraAcSwingOff);
    ac.setSwingH(kElectraAcSwingOff);

    Serial.printf("[IR] Driver Started. RX:%d, TX:%d\n", PIN_IR_RX, PIN_IR_TX);
}

// 专门控制空调的函数
void App_IR_Control_AC(bool power, uint8_t temp)
{
    Serial.printf("IR: Setting AC Power=%d, Temp=%d\n", power, temp);

    // 暂停接收，防止干扰发送
    // _irRecv->disableIRIn(); 

    // 1. 设置电源
    if (power) {
        ac.on();
    } else {
        ac.off();
    }

    // 2. 设置模式
    ac.setMode(kElectraAcCool);

    // 3. 设置风速
    ac.setFan(kElectraAcFanAuto);

    // 4. 设置温度
    ac.setTemp(temp);

    // 5. 发送信号
    ac.send();
    
    // 恢复接收
    // _irRecv->enableIRIn();

    Serial.println("IR: Signal Sent (Electra Protocol)!");
    Serial.println(ac.toString().c_str()); 
}

// 测试函数
void App_IR_Test_Send(void)
{
    App_IR_Control_AC(true, 20);
}

void AppIR::loop() {
    if (_irRecv->decode(&_results)) {
        if (_results.value != kRepeat) {
            IREvent evt;
            memset(&evt, 0, sizeof(IREvent));
            evt.protocol = _results.decode_type;
            evt.bits = _results.bits;
            evt.value = _results.value;
            evt.isAC = false;

            if (_results.decode_type == COOLIX || _results.bits > 64) {
                evt.isAC = true;
                int byteCount = _results.bits / 8;
                if (_results.bits % 8 != 0) byteCount++; 
                if (byteCount > IR_STATE_SIZE) byteCount = IR_STATE_SIZE; 
                for (int i = 0; i < byteCount; i++) evt.state[i] = _results.state[i];
                Serial.printf("[IR] AC Signal Detected (Protocol: %s)\n", typeToString(_results.decode_type).c_str());
            } else {
                Serial.printf("[IR] Normal Signal: 0x%llX\n", _results.value);
            }
            if (IRQueue_Handle != NULL) xQueueSend(IRQueue_Handle, &evt, 0);
        } 
        _irRecv->resume(); 
    }
}

// 修改后的 sendNEC：为了兼容性，临时创建一个 sender
void AppIR::sendNEC(uint32_t data) {
    Serial.printf("[IR] Sending NEC: 0x%08X\n", data);
    
    // 临时创建一个 sender 来发送 NEC，避免全局冲突
    // 注意：频繁创建销毁开销较大，但对于偶尔控制灯是没问题的
    IRsend tempSender(PIN_IR_TX);
    tempSender.begin();
    tempSender.sendNEC(data, 32);
    
    // 发送完 NEC 后，必须重新初始化 AC 对象，否则 AC 下次可能发不出信号
    // 因为 tempSender 销毁时可能会重置引脚状态
    ac.begin(); 

    vTaskDelay(pdMS_TO_TICKS(20)); 
    _irRecv->enableIRIn(); 
}

void AppIR::sendCoolix(uint32_t data) {
    // 同样的逻辑
    IRsend tempSender(PIN_IR_TX);
    tempSender.begin();
    tempSender.sendCOOLIX(data);
    ac.begin(); // 恢复 AC 驱动
    
    vTaskDelay(pdMS_TO_TICKS(50)); 
    _irRecv->enableIRIn(); 
    Serial.println("[IR] Coolix Sent.");
}