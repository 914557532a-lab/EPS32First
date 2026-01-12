/**
 * @file App_Audio.cpp
 * @brief 音频实现 - 移植了高音质优化算法 (APLL + SoftFilter)
 */
#include "App_Audio.h"
#include "Pin_Config.h" // 务必包含此文件以获取正确的引脚定义

// --- 引入核心库 ---
#include "AudioTools.h"
#include "AudioBoard.h"

AppAudio MyAudio;

// --- 全局对象实例化 ---
static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s; // AudioTools I2S流

// --- 播放任务参数结构体 ---
struct ToneParams {
    int freq;
    int duration;
};

// 辅助：写静音
void writeSilence(int ms) {
    // 字节数 = 采样率 * 通道数(2) * 位深(2) * 时间 / 1000
    int bytes = (AUDIO_SAMPLE_RATE * 4 * ms) / 1000;
    uint8_t silence[256] = {0}; 
    while (bytes > 0) {
        int to_write = (bytes > 256) ? 256 : bytes;
        i2s.write(silence, to_write);
        bytes -= to_write;
    }
}

// 任务包装函数：播放提示音
void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    
    // 使用全局定义的采样率
    const int sample_rate = AUDIO_SAMPLE_RATE;
    const int amplitude = 10000; 
    
    int total_samples = (sample_rate * p->duration) / 1000;
    
    // 缓冲区: 128帧 * 2通道 * 2字节
    int16_t sample_buffer[256]; 
    
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            // 生成正弦波
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            // 双声道填充
            sample_buffer[2*j] = val;     
            sample_buffer[2*j+1] = val;   
        }
        i2s.write((uint8_t*)sample_buffer, batch * 4);
        
        // 防止看门狗
        if(i % 1024 == 0) delay(1); // vTaskDelay(1)
    }

    // 播放结束写一点静音防止爆破音
    writeSilence(20);

    free(p);
    vTaskDelete(NULL); 
}

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

// ==========================================================
// Class Implementation
// ==========================================================

void AppAudio::init() {
    Serial.println("[Audio] Init Start (High Quality Mode)...");

    // 1. 强制拉高 PA_EN (GPIO 18)
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, HIGH);

    // 2. 配置 DriverPins (严格使用 Pin_Config.h 中的定义)
    // I2C: SDA=47, SCL=48
    my_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0); 
    // I2S: MCLK=42, BCLK=41, LRCK=39, DOUT=38, DIN=40
    my_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    // PA Enable
    my_pins.addPin(PinFunction::PA, PIN_PA_EN, PinLogic::Output);

    // 3. 配置 ES8311 Codec 参数
    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_ALL; // 如果录音有杂音，可尝试改为 ADC_INPUT_LINE1
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    
    // [优化点] Codec 内部时钟设置
    // 即使我们用 24k 播放，Codec 内部滤波器带宽设为 32k 或 48k 通常听感更好
    cfg.i2s.rate = RATE_32K; 
    cfg.i2s.fmt = I2S_NORMAL; 

    // 4. 初始化芯片
    if (board.begin(cfg)) {
        Serial.println("[Audio] ES8311 Init OK!");
    } else {
        Serial.println("[Audio] ES8311 Init FAILED! (Check I2C)");
    }

    // 5. 设置音量
    board.setVolume(60);       
    board.setInputVolume(85); 

    // 6. 配置 I2S 数据流 (关键优化)
    auto config = i2s.defaultConfig(RXTX_MODE); 
    
    // 引脚映射
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    
    // [关键优化] 采样率与 APLL
    config.sample_rate = AUDIO_SAMPLE_RATE; // 24000
    config.bits_per_sample = 16;
    config.channels = 2;       // 强制立体声
    config.use_apll = true;    // [重点] 开启 APLL 以获得高保真时钟
    
    if (i2s.begin(config)) {
        Serial.printf("[Audio] I2S Stream Started (Rate: %d, APLL: On)\n", AUDIO_SAMPLE_RATE);
    } else {
        Serial.println("[Audio] I2S Stream FAILED!");
    }

    // 7. 申请录音内存
    if (psramFound()) {
        record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
        Serial.println("[Audio] PSRAM buffer allocated");
    } else {
        record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);
        Serial.println("[Audio] SRAM buffer allocated");
    }
    
    // 初始化后播放一声提示音
    playToneAsync(1000, 200);
}

void AppAudio::setVolume(uint8_t vol) {
    board.setVolume(vol);
}

void AppAudio::setMicGain(uint8_t gain) {
    board.setInputVolume(gain);
}

void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq;
        params->duration = duration_ms;
        xTaskCreate(playTaskWrapper, "PlayTask", 4096, params, 2, NULL);
    }
}

void AppAudio::startRecording() {
    if (isRecording) return;
    if (!record_buffer) return;

    record_data_len = 44; // 预留44字节WAV头
    isRecording = true;
    
    // 录音任务
    xTaskCreate(recordTaskWrapper, "RecTask", 8192, this, 10, &recordTaskHandle);
    Serial.println("[Audio] Start Recording...");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); 
    
    uint32_t pcm_size = record_data_len - 44;
    Serial.printf("[Audio] Record Stop. Size: %d\n", pcm_size);
    
    // 使用全局采样率生成 WAV 头
    createWavHeader(record_buffer, pcm_size, AUDIO_SAMPLE_RATE, 16, 2);
}

void AppAudio::_recordTask(void *param) {
    const size_t read_size = 1024; 
    uint8_t temp_buf[read_size]; 

    while (isRecording) {
        size_t bytes_read = i2s.readBytes(temp_buf, read_size);

        if (bytes_read > 0) {
            if (record_data_len + bytes_read < MAX_RECORD_SIZE) {
                memcpy(record_buffer + record_data_len, temp_buf, bytes_read);
                record_data_len += bytes_read;
            } else {
                isRecording = false; 
            }
        } else {
            vTaskDelay(1);
        }
    }
}

// [核心优化] 流式播放：增加滤波和平滑处理
void AppAudio::playStream(WiFiClient *client, int length) {
    if (!client || length <= 0) return;

    Serial.printf("[Audio] Playing Stream: %d bytes (Soft Mode)\n", length);
    
    // 先静音一下，消除可能的爆音
    writeSilence(20);

    // 缓冲区
    const int buff_size = 2048;
    uint8_t buff[buff_size]; 
    int16_t stereo_buff[buff_size]; // 用于扩展立体声

    // 滤波相关变量
    static int16_t last_sample_L = 0; 
    float volume_scale = 0.8; // 稍微降低一点数字增益，防止爆音

    int remaining = length;
    
    while (remaining > 0 && client->connected()) {
        // 计算本次读取量 (注意：不要超过 buff 大小的一半，因为我们需要扩展成立体声)
        int max_read = (remaining > (buff_size / 2)) ? (buff_size / 2) : remaining;
        
        // 健壮的读取循环 (参考代码逻辑)
        int bytesIn = 0;
        unsigned long startWait = millis();
        while (bytesIn < max_read && millis() - startWait < 1000) {
             if (client->available()) {
                 bytesIn += client->read(buff + bytesIn, max_read - bytesIn);
             } else {
                 delay(1);
             }
        }
        
        if (bytesIn == 0) break; // 读取超时或断开

        // --- 音频处理 (单声道 -> 立体声 + 滤波) ---
        int sample_count = bytesIn / 2; // 16bit = 2bytes per sample
        int16_t *pcm_samples = (int16_t*)buff;

        for (int i = 0; i < sample_count; i++) {
            // 简单低通滤波 (消除 32k->24k 降速播放可能产生的高频噪声)
            int16_t raw = pcm_samples[i];
            int16_t filtered = (raw + last_sample_L) >> 1; 
            last_sample_L = raw; 

            // 应用音量并在 int16 范围内截断
            int16_t val = (int16_t)(filtered * volume_scale);
            
            // 填充到立体声缓冲
            stereo_buff[i*2]     = val; 
            stereo_buff[i*2 + 1] = val; 
        }

        // 写入 I2S (字节数 = 样本数 * 2通道 * 2字节)
        i2s.write((uint8_t*)stereo_buff, sample_count * 4);
        
        remaining -= bytesIn;
    }
    
    // 播放结束写静音
    writeSilence(50);
    Serial.println("[Audio] Stream End");
}

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
    header[20] = 1; header[21] = 0; 
    header[22] = numChannels; header[23] = 0; 
    header[24] = (uint8_t)(sampleRate & 0xFF);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xFF);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
    header[28] = (uint8_t)(byteRate & 0xFF);
    header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
    header[30] = (uint8_t)((byteRate >> 16) & 0xFF);
    header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
    header[32] = (uint8_t)(numChannels * (sampleBits / 8)); header[33] = 0; 
    header[34] = sampleBits; header[35] = 0;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(totalDataLen & 0xFF);
    header[41] = (uint8_t)((totalDataLen >> 8) & 0xFF);
    header[42] = (uint8_t)((totalDataLen >> 16) & 0xFF);
    header[43] = (uint8_t)((totalDataLen >> 24) & 0xFF);
}

// C 接口
void Audio_Play_Click() { MyAudio.playToneAsync(1000, 100); }
void Audio_Record_Start() { MyAudio.startRecording(); }
void Audio_Record_Stop() { MyAudio.stopRecording(); }