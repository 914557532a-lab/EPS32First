#include "App_IR.h"

AppIR MyIR;

const uint16_t kCaptureBufferSize = 1024; 
const uint8_t kTimeout = 20; // 稍微调大一点超时，防止空调长码被截断

extern QueueHandle_t IRQueue_Handle; 

void AppIR::init() {
    Serial.println("[IR] Initializing...");

    _irRecv = new IRrecv(PIN_IR_RX, kCaptureBufferSize, kTimeout, true);
    _irRecv->enableIRIn(); 

    _irSend = new IRsend(PIN_IR_TX);
    _irSend->begin(); 

    Serial.printf("[IR] Driver Started. RX:%d, TX:%d\n", PIN_IR_RX, PIN_IR_TX);
}

void AppIR::loop() {
    if (_irRecv->decode(&_results)) {
        
        // 过滤重复码 (NEC Repeat)
        if (_results.value != kRepeat) {
            
            IREvent evt;
            // 清空结构体，防止脏数据
            memset(&evt, 0, sizeof(IREvent));

            evt.protocol = _results.decode_type;
            evt.bits = _results.bits;
            evt.value = _results.value; // 默认存一下 value
            evt.isAC = false;

            // --- 核心判断逻辑 ---
            // 判断是否为 AUX 协议或者位数特别长 (>64位)
            if (_results.decode_type == COOLIX || _results.bits > 64) {
                evt.isAC = true;
                
                // 将库里的 state 数组复制到我们的结构体中
                // 注意：IRremoteESP8266 库解码 AC 时会把数据放在 _results.state[] 中
                // 且以字节为单位。我们需要计算需要复制多少字节。
                int byteCount = _results.bits / 8;
                if (_results.bits % 8 != 0) byteCount++; // 处理非整字节
                if (byteCount > IR_STATE_SIZE) byteCount = IR_STATE_SIZE; // 防止溢出

                // 复制数据
                for (int i = 0; i < byteCount; i++) {
                    evt.state[i] = _results.state[i];
                }
                
                Serial.printf("[IR] 收到空调信号 (Protocol: %s, Bits: %d)\n", typeToString(_results.decode_type).c_str(), _results.bits);
                Serial.print("[IR] Hex Data: {");
                for(int i=0; i<byteCount; i++) {
                    Serial.printf("0x%02X", evt.state[i]);
                    if(i < byteCount - 1) Serial.print(", ");
                }
                Serial.println("}");

            } else {
                // 普通电视遥控器
                Serial.printf("[IR] 收到普通信号: 0x%llX (Bits: %d)\n", _results.value, _results.bits);
            }

            // 发送到队列
            if (IRQueue_Handle != NULL) {
                xQueueSend(IRQueue_Handle, &evt, 0);
            }
        } 
        
        _irRecv->resume(); 
    }
}

// 发送普通 NEC (不变)
void AppIR::sendNEC(uint32_t data) {
    Serial.printf("[IR] Sending NEC: 0x%08X\n", data);
    _irSend->sendNEC(data, 32);
    vTaskDelay(pdMS_TO_TICKS(20)); 
    _irRecv->enableIRIn(); 
}

// 【新增】发送 AUX 空调指令
void AppIR::sendCoolix(uint32_t data) {
    Serial.printf("[IR] Sending Coolix: 0x%X\n", data);
    
    // Coolix 协议通常发送 24位 (3字节) 的数据
    // 注意：IRremoteESP8266 的 sendCoolix 默认发送 24位
    _irSend->sendCOOLIX(data);
    
    // 发完恢复接收
    vTaskDelay(pdMS_TO_TICKS(50)); 
    _irRecv->enableIRIn(); 
    Serial.println("[IR] Coolix Sent.");
}