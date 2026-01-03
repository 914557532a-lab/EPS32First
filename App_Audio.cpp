#include "App_Audio.h"
#include <Wire.h>
#include <math.h>
#include "Pin_Config.h" // 确保这里包含你的引脚定义

AppAudio MyAudio;

// --- C 接口实现 (连接 UI 和 C++ 类) ---
//封装起来后续要修改提示音的声音直接改一处即可
void Audio_Play_Click() {//
    // 播放 800Hz 提示音，持续 200ms
    MyAudio.playToneAsync(800, 200);
}

void Audio_Record_Start() {//音频录制开启
    MyAudio.startRecording();
}

void Audio_Record_Stop() {//音频录制结束
    MyAudio.stopRecording();
}

// ------------------------------------

// 简单的播放参数结构体
struct ToneParams {//主要用于提示音，后续升级可以删除，直接导入短音频到flash
    int freq;//频率
    int duration;//持续时间
};

// ES8311 初始化数据
const uint8_t es8311_init_data[][2] = {//初始配置文件
    {0x45, 0x00}, {0x01, 0x30}, {0x02, 0x10}, {0x02, 0x00}, 
    {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00}, {0x0B, 0x00}, 
    {0x0C, 0x00}, {0x0D, 0x01}, {0x10, 0x1F}, {0x11, 0x7F}, 
    {0x12, 0x00}, {0x13, 0x00}, {0x14, 0x1A}, {0x32, 0xBF}, 
    {0x37, 0x08}, {0x00, 0x80},
};

void AppAudio::init() {//初始化函数
    Serial.println("[Audio] Init Start...");
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);//启动 I2C 总线，并指定具体哪两个引脚作为数据线（SDA）和时钟线（SCL）
    
    pinMode(PIN_PA_EN, OUTPUT);//将功放的引脚配置为输出模式
    digitalWrite(PIN_PA_EN, LOW);//默认关闭功放或使其进入静音，不然上电可能会又爆音

    i2s_config_t i2s_config = {//I2C配置
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),//将 ESP32 设为“主机”。它将产生 BCLK（位时钟）和 WS（字时钟），控制数据传输的节奏
        .sample_rate = 16000,//采样率为 16kHz
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,//每个采样点用 16 位数据表示
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,//双声道
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,//使用标准的 I2S 通信格式
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,//给 I2S 外设分配一个优先级为 1 级
        .dma_buf_count = 8,//8个缓冲区
        .dma_buf_len = 64,//每个缓冲区的大小
        .use_apll = true,//开启音频锁相环
        .tx_desc_auto_clear = true,//当没有新音频数据要发送时，它会自动发送静音数据，防止扬声器产生由于电流残留导致的噪音或异常声
        .fixed_mclk = 0//不锁定主时钟频率，允许系统根据采样率自动计算并生成最合适的时钟
    };
    //I2S 引脚映射配置
    i2s_pin_config_t pin_config = {
        .mck_io_num = PIN_I2S_MCLK,//主时钟，可以作为ES8311时钟的参考
        .bck_io_num = PIN_I2S_BCLK,//位时钟，决定传输比特的节奏
        .ws_io_num = PIN_I2S_LRCK,//左右声道切换时钟
        .data_out_num = PIN_I2S_DOUT,//音频播放数据线
        .data_in_num = PIN_I2S_DIN//音频录音数据线
    };

    i2s_driver_install(i2s_num, &i2s_config, 0, NULL);//安装到对应I2C上
    i2s_set_pin(i2s_num, &pin_config);//设置对应引脚
    i2s_zero_dma_buffer(i2s_num);//清空缓冲区（静音初始化）

    // ES8311 Init
    writeReg(0x00, 0x1F); delay(20); writeReg(0x00, 0x00);
    for (int i = 0; i < sizeof(es8311_init_data) / 2; i++) {
        writeReg(es8311_init_data[i][0], es8311_init_data[i][1]);
    }
    
    setVolume(60);
    setMicGain(0xBF); 
    digitalWrite(PIN_PA_EN, HIGH);

    // --- 新增：申请录音内存 (尝试申请 PSRAM) ---
    record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    if (record_buffer == NULL) {
        Serial.println("[Audio] Failed to allocate PSRAM for recording! Trying malloc...");
        record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE); // 如果没有 PSRAM，尝试普通 RAM (可能不够)
    } else {
        Serial.printf("[Audio] Allocated %d bytes in PSRAM for recording.\n", MAX_RECORD_SIZE);
    }
    
    Serial.println("[Audio] Init Done.");
}

// ---------------- 播放功能 ----------------

// FreeRTOS 任务包装器：播放声音（主要是提示音）
void playTaskWrapper(void *param) {//创立一个通用的指针地址
    ToneParams *p = (ToneParams*)param;//定义一个ToneParams类型的指针P去读取param（也强制转化成ToneParams这个结构体类型的参数）
    // 生成并播放正弦波
    size_t bytes_written;
    int sample_rate = 44100;
    int samples = (sample_rate * p->duration) / 1000;//p->duration传递参数里读出的持续时间，每秒采样点数*持续时间，再/1000，计算出每毫秒的点数
    
    // 简单动态生成波形，为了省内存不一次性 malloc 大数组
    // 每次处理 1024 个样本
    int16_t chunk[1024];  //1024个
    int chunk_size = 1024; //定义chunk的大小
    int total_processed = 0;//统计装了多少个了
    
    while(total_processed < samples) {//如果没装满就继续装
        int to_process = (samples - total_processed) > (chunk_size/2) ? (chunk_size/2) : (samples - total_processed);
        //下面这个函数用来产生正弦波
        for(int i=0; i<to_process; i++) {
            int16_t val = (int16_t)(10000 * sin(2 * PI * p->freq * (total_processed + i) / sample_rate));
            chunk[i*2] = val;     // Left
            chunk[i*2+1] = val;   // Right
        }
        
        i2s_write(I2S_NUM_0, chunk, to_process * 4, &bytes_written, portMAX_DELAY);
        total_processed += to_process;
    }

    // 播放结束，清理内存并自杀
    free(p);
    vTaskDelete(NULL); 
}

void AppAudio::playToneAsync(int freq, int duration_ms) {
    // 创建参数结构体 (必须在堆上分配，传给任务)
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));//计算ToneParams这个结构体的大小并且申请一块内存进行存储，申请出来的地址也是结构体ToneParams指针的形式，然后用符合ToneParams结构体的指针params去指向
    if(params) {
        params->freq = freq;//把freq塞到params的freq里
        params->duration = duration_ms;//把duration_ms塞到params的freq里
        // 创建一个临时任务来播放声音，优先级设低一点以免卡住 UI
        xTaskCreate(playTaskWrapper, "PlayTask", 4096, params, 1, NULL);
        Serial.println("[Audio] Play task started");
    }
}

// ---------------- 录音功能 ----------------

// FreeRTOS 任务包装器：录音
void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); // 调用类成员函数
    vTaskDelete(NULL);
}

void AppAudio::_recordTask(void *param) {
    size_t bytes_read;
    
    // I2S 读取缓冲区 (栈上分配)
    // 我们一次读 1024 个采样点 (每个点2字节 * 2声道 = 4096字节)
    const int samples_per_chunk = 512;
    int16_t i2s_read_buff[samples_per_chunk * 2]; // *2 是因为双声道 (L+R)

    while (isRecording) {
        // 1. 从 I2S 读取双声道数据 (16kHz, Stereo, 16bit)
        i2s_read(i2s_num, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            // 计算读到了多少个“帧” (一个帧包含 L 和 R 两个 int16)
            int frames = bytes_read / 4; // 4 bytes per frame (16bit * 2 channels)

            // 检查 PSRAM 是否够放 (注意：写入的数据量只有读取的一半)
            if (record_data_len + (frames * 2) >= MAX_RECORD_SIZE) {
                Serial.println("[Audio] Buffer Full!");
                isRecording = false;
                break;
            }

            // 2. 【修改点3】软件转单声道：只取左声道数据写入 PSRAM
            // i2s_read_buff 排列是: [L0, R0, L1, R1, L2, R2 ...]
            // 我们只需要 L，所以跳着取
            int16_t *psram_ptr = (int16_t *)(record_buffer + record_data_len);
            
            for (int i = 0; i < frames; i++) {
                // 取第 i 个左声道数据 (i*2)
                psram_ptr[i] = i2s_read_buff[i * 2]; 
            }

            // 更新已写入长度 (写入了 frames 个 int16，每个2字节)
            record_data_len += (frames * 2);
        }
        else {
            vTaskDelay(1);
        }
    }
    
    recordTaskHandle = NULL;
    vTaskDelete(NULL);
}

void AppAudio::startRecording() {
    if (isRecording) {
        Serial.println("[Audio] Already recording...");
        return;
    }
    
    // --- 新增：重置录音长度，预留 44 字节 WAV 头 ---
    if (record_buffer != NULL) {
        record_data_len = 44; 
    } else {
        Serial.println("[Audio] No buffer allocated!");
        return;
    }

    isRecording = true;
    xTaskCreate(recordTaskWrapper, "RecTask", 4096, this, 2, &recordTaskHandle);
}

void AppAudio::stopRecording() {
    isRecording = false; // 设置标志位，任务会在下一次循环检测时退出
    Serial.println("[Audio] Stopping recording...");
    delay(50); // 等待任务结束
    uint32_t pcm_size = record_data_len - 44;
    Serial.printf("[Audio] Stop. PCM Size: %d bytes (Mono, 16k)\n", pcm_size);
    createWavHeader(record_buffer, pcm_size, 16000, 16, 1);
}
// --- 新增：WAV 头部生成函数实现 ---
void AppAudio::createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels) {
    uint32_t byteRate = sampleRate * numChannels * (sampleBits / 8);
    uint32_t totalFileSize = totalDataLen + 44 - 8;
    
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(totalFileSize & 0xFF);
    header[5] = (uint8_t)((totalFileSize >> 8) & 0xFF);
    header[6] = (uint8_t)((totalFileSize >> 16) & 0xFF);
    header[7] = (uint8_t)((totalFileSize >> 24) & 0xFF);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0; // PCM format
    header[22] = numChannels; header[23] = 0;
    header[24] = (uint8_t)(sampleRate & 0xFF);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
    header[28] = (uint8_t)(byteRate & 0xFF);
    header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
    header[30] = (uint8_t)((byteRate >> 16) & 0xFF);
    header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
    header[32] = (uint8_t)(numChannels * (sampleBits / 8)); header[33] = 0; // Block align
    header[34] = sampleBits; header[35] = 0;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(totalDataLen & 0xFF);
    header[41] = (uint8_t)((totalDataLen >> 8) & 0xFF);
    header[42] = (uint8_t)((totalDataLen >> 16) & 0xFF);
    header[43] = (uint8_t)((totalDataLen >> 24) & 0xFF);
}
// ---------------- 辅助函数 ----------------

void AppAudio::setVolume(uint8_t vol) {
    if (vol > 100) vol = 100;
    uint8_t reg_val = map(vol, 0, 100, 0, 0xBF);
    writeReg(0x32, reg_val);
}

void AppAudio::setMicGain(uint8_t gain) {
    // 0x14 ADC Volume
    writeReg(0x14, gain); 
}

void AppAudio::writeReg(uint8_t reg, uint8_t data) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(data);
    Wire.endTransmission();
}

uint8_t AppAudio::readReg(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(ES8311_ADDR, 1);
    if (Wire.available()) return Wire.read();
    return 0;
}