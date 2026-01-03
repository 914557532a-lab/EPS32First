#include "App_4G.h"

App4G My4G;

// --- 辅助函数：RTOS 专用延时 ---
// 作用：让当前任务“睡觉”，把 CPU 让给 UI 任务
void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void App4G::init() {
    // 1. 初始化控制引脚
    pinMode(PIN_4G_PWR_EN, OUTPUT);  
    pinMode(PIN_4G_PWR_KEY, OUTPUT); 
    
    // 关于 GPIO 14 (背光冲突) 的处理：
    // 建议：如果不需要读取网络状态，或者为了防止背光闪烁，
    // 可以暂时注释掉下面这行，或者只在需要读取时临时设为 INPUT
    pinMode(PIN_4G_NETSTATE, INPUT); 

    // 默认引脚状态
    digitalWrite(PIN_4G_PWR_EN, LOW);   
    digitalWrite(PIN_4G_PWR_KEY, HIGH); 

    // 2. 初始化串口
    _serial->begin(115200, SERIAL_8N1, PIN_4G_RX, PIN_4G_TX);

    Serial.println("[4G] Hardware GPIO Initialized.");
    
    // 【重要修改】
    // 这里删除了 powerOn(); 
    // 原因：不要在 init 里跑耗时代码，把它留给 Task 去做。
}

void App4G::powerOn() {
    Serial.println("[4G] Powering ON (Task Context)...");

    // 步骤 1: 打开主电源
    digitalWrite(PIN_4G_PWR_EN, HIGH);
    delay_ms(500); // 【改】让出 CPU 0.5秒

    // 步骤 2: 拉低 PWRKEY
    Serial.println("[4G] Toggling PWRKEY...");
    digitalWrite(PIN_4G_PWR_KEY, LOW);  
    delay_ms(2000); // 【改】让出 CPU 2秒 (UI 依然流畅)
    digitalWrite(PIN_4G_PWR_KEY, HIGH); 

    // 步骤 3: 等待启动
    Serial.println("[4G] Waiting for module boot...");
    delay_ms(3000); // 【改】让出 CPU 3秒

    // 步骤 4: 握手测试
    bool alive = false;
    for(int i=0; i<5; i++) {
        String resp = sendAT("AT");
        if (resp.indexOf("OK") != -1) {
            alive = true;
            Serial.println("[4G] Module is Alive!");
            break;
        }
        delay_ms(500);
    }

    if (alive) {
        // 【新增】模块活了之后，顺手把 IMEI 取回来
        fetchIMEI();
    } else {
        Serial.println("[4G] Error: No response.");
    }
}
// 【新增】实现获取逻辑
void App4G::fetchIMEI() {
    // 发送查询指令
    // AT+CGSN 通常用于查询 IMEI
    String response = sendAT("AT+CGSN");
    
    // 返回格式通常是:
    // AT+CGSN
    // 869999041234567
    // OK
    
    // 我们需要简单的字符串处理，提取出那串数字
    // 1. 去掉首尾空格
    response.trim();
    
    // 2. 简单的解析逻辑：
    // 找到第一个换行符之后，和最后一个 OK 之前的内容
    // 这里做一个简化的提取，通常 IMEI 是 15 位数字
    int start = 0;
    // 如果包含命令回显，跳过命令
    if (response.startsWith("AT+CGSN")) {
        start = response.indexOf('\n'); 
    }
    
    String cleanStr = "";
    for (int i = start; i < response.length(); i++) {
        char c = response.charAt(i);
        // 只提取数字
        if (isDigit(c)) {
            cleanStr += c;
        }
    }

    // 只有长度合理才更新
    if (cleanStr.length() >= 14) {
        _cachedIMEI = cleanStr;
        Serial.println("[4G] IMEI Cached: " + _cachedIMEI);
    } else {
        Serial.println("[4G] Failed to parse IMEI, Raw: " + response);
        _cachedIMEI = "Unknown_Device"; // 默认值
    }
}

// 【新增】对外接口
String App4G::getIMEI() {
    // 如果还没获取到，尝试获取一次（防止初始化时漏掉）
    if (_cachedIMEI == "") fetchIMEI();
    return _cachedIMEI;
}

void App4G::hardwareReset() {
    Serial.println("[4G] Performing Power Cycle Reset...");
    digitalWrite(PIN_4G_PWR_EN, LOW);  
    delay_ms(1000); // 【改】
    digitalWrite(PIN_4G_PWR_EN, HIGH); 
    delay_ms(500);  // 【改】
    
    // 重新执行开机逻辑
    digitalWrite(PIN_4G_PWR_KEY, LOW);
    delay_ms(2000); // 【改】
    digitalWrite(PIN_4G_PWR_KEY, HIGH);
}

String App4G::sendAT(String command, uint32_t timeout_ms) {
    // 1. 清空接收缓存
    while (_serial->available()) _serial->read();

    // 2. 发送指令
    _serial->println(command);
    
    // 3. 等待并读取回应
    String response = "";
    uint32_t start = millis();
    
    // 这里的 while 循环会阻塞当前任务 (Task4G)，这是正常的。
    // 只要不阻塞其他任务 (UI) 就行。
    while (millis() - start < timeout_ms) {
        if (_serial->available()) {
            char c = _serial->read();
            response += c;
        } else {
            // 【优化】
            // 如果串口没数据，不要死循环空转，休息 1 个 Tick (约1ms)
            // 这能降低 CPU 占用率，防止看门狗报警
            vTaskDelay(1); 
        }
        
        if (response.endsWith("OK\r\n") || response.endsWith("ERROR\r\n")) {
            // 给一点点缓冲时间让缓冲区清空
            delay_ms(10); // 【改】等待 10ms
            while(_serial->available()) response += (char)_serial->read();
            break;
        }
    }
    return response;
}

void App4G::loopPassthrough() {
    // 透传模式不要加太多延时，保证速度
    if (Serial.available()) {
        _serial->write(Serial.read());
    }
    if (_serial->available()) {
        Serial.write(_serial->read());
    }
}

bool App4G::isNetConnected() {
    return digitalRead(PIN_4G_NETSTATE);
}