#include "App_Audio.h"
#include <Wire.h>
#include <math.h>
#include "Pin_Config.h" 
#include "driver/gpio.h" // 引入 GPIO 驱动，用于复位引脚

AppAudio MyAudio;

// --- C 接口实现 ---
void Audio_Play_Click() {
    MyAudio.playToneAsync(800, 200);
}

void Audio_Record_Start() {
    MyAudio.startRecording();
}

void Audio_Record_Stop() {
    MyAudio.stopRecording();
}

// ------------------------------------

struct ToneParams {
    int freq;
    int duration;
};

// ES8311 初始化配置
// 替换原有的 es8311_init_data
// 在 App_Audio.cpp 顶部替换这个数组
// 修复版配置数组：
// 1. 恢复 0x01=0x30 (解决“死寂/无声”问题，恢复时钟)
// 2. 修改 0x09/0x0B=0x0C (解决“电流音/噪声”问题，强制 I2S Philips 格式)
// 3. 增加 0x0E=0x02 (解决“录音为空”问题，开启 ADC 调制器)

const uint8_t es8311_init_data[][2] = {
    // --- 1. 复位 (参考代码设为 0x80) ---
    {0x00, 0x80}, // Clock State Machine On, Slave Mode (ESP32是Master)

    // --- 2. 时钟管理 ---
    // 0x30: 开启 MCLK 和 BCLK 内部时钟 (参考代码初始化值)
    {0x01, 0x30}, 

    // --- 3. 关键：时钟分频系数 (由 es8311.c 针对 16k/4M MCLK 计算得出) ---
    // 这部分是消除“滋滋声”的核心
    {0x02, 0x00}, // Pre-div=1, Mult=1
    {0x03, 0x10}, // ADC OSR=16, Double Speed=0
    {0x04, 0x10}, // DAC OSR=16
    {0x05, 0x00}, // ADC/DAC Div=1
    
    // BCLK 分频系数 (重要!)
    // 4.096MHz / 16kHz = 256.  256 / 4 = 64 BCLKs per LRCK.
    // 0x03 = (4 - 1). 
    {0x06, 0x03}, 
    
    // LRCK 分频系数 (重要!)
    {0x07, 0x00}, // LRCK High
    {0x08, 0xFF}, // LRCK Low (255) - 参考代码使用的值

    // --- 4. 格式配置 (I2S, 16bit) ---
    // 0x00 对应 I2S 标准格式, 16位数据
    {0x09, 0x00}, // DAC SDP In
    {0x0A, 0x00}, // ADC SDP Out (注意：参考代码这里用的是 0x0A 而不是 0x09)

    // --- 5. 系统设置 ---
    {0x0B, 0x00}, 
    {0x0C, 0x00},
    {0x0D, 0x01}, // Power Up Analog
    {0x0E, 0x02}, // Enable Analog PGA & ADC Modulator
    {0x10, 0x1F}, // ALC Control
    {0x11, 0x7F}, // ALC Max Gain

    // --- 6. 启动序列 (参考 es8311_start 函数) ---
    // 这里的设置对录音至关重要
    {0x12, 0x00}, // Master/Slave config
    {0x13, 0x10}, // Cross talk (参考代码值)
    {0x14, 0x1A}, // 开启模拟麦克风 (Dmic Disable), Max Gain
    {0x15, 0x40}, // ADC Control (Soft Ramp?) - 参考代码值!
    {0x16, 0x24}, // Mic Gain (参考代码值)
    {0x17, 0xBF}, // ADC Volume (Max)
    
    {0x1B, 0x0A}, // ADC Automute settings (参考代码)
    {0x1C, 0x6A}, // ADC Automute settings (参考代码)

    // --- 7. DAC 设置 ---
    {0x31, 0x00}, // Unmute (Bit 6=0)
    {0x32, 0xBF}, // DAC Volume
    {0x37, 0x48}, // DAC Control (参考代码设为 0x48, 之前是 0x08)
    
    // --- 8. GPIO/其他 ---
    {0x45, 0x00}, 
};

void AppAudio::init() {
    Serial.println("[Audio] Init Start...");

    // 1. I2C 初始化
    if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.println("[Audio] ERROR: I2C Init Failed!");
        return; 
    }
    
    // 2. 先关闭功放
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);

    // 3. 强制复位 I2S 引脚
    gpio_reset_pin((gpio_num_t)PIN_I2S_MCLK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_BCLK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_LRCK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_DOUT);
    gpio_reset_pin((gpio_num_t)PIN_I2S_DIN);

    // 4. 配置 I2S (保持 16kHz, Philips 格式)
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, 
        .communication_format = I2S_COMM_FORMAT_STAND_I2S, // Philips 标准
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 16000 * 256 // 4.096 MHz
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = PIN_I2S_MCLK,
        .bck_io_num = PIN_I2S_BCLK,
        .ws_io_num = PIN_I2S_LRCK,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num = PIN_I2S_DIN
    };

    if (i2s_driver_install(i2s_num, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("[Audio] I2S Install Failed!");
        return;
    }
    i2s_set_pin(i2s_num, &pin_config);
    i2s_zero_dma_buffer(i2s_num);

    // 5. 写入 ES8311 寄存器
    Serial.println("[Audio] Configuring ES8311...");
    for (int i = 0; i < sizeof(es8311_init_data) / 2; i++) {
        writeReg(es8311_init_data[i][0], es8311_init_data[i][1]);
        delay(1); 
    }
    
    int reg09 = readReg(0x09);
    writeReg(0x09, reg09 & 0xBF); // DAC Unmute (播放)
    
    int reg0A = readReg(0x0A);
    writeReg(0x0A, reg0A & 0xBF); // ADC Unmute (录音)
    
    // 再次确认音量和增益
    setVolume(75); 
    setMicGain(0xBF); 

    // 6. 开启功放 (加一点延时等待 Codec 稳定)
    delay(50);
    digitalWrite(PIN_PA_EN, HIGH); 

    // 7. 内存分配
    record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    if (record_buffer == NULL) {
        record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);
    }
    
    Serial.println("[Audio] Init Done.");
}

// ---------------- 播放功能 ----------------

void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    size_t bytes_written;
    
    // --- 修复: 必须与 I2S 初始化时的 sample_rate (16000) 一致 ---
    int sample_rate = 16000; 
    
    int samples = (sample_rate * p->duration) / 1000;
    
    int16_t chunk[1024];  
    int chunk_size = 1024; 
    int total_processed = 0;
    
    while(total_processed < samples) {
        int to_process = (samples - total_processed) > (chunk_size/2) ? (chunk_size/2) : (samples - total_processed);
        for(int i=0; i<to_process; i++) {
            // 生成正弦波
            int16_t val = (int16_t)(10000 * sin(2 * PI * p->freq * (total_processed + i) / sample_rate));
            chunk[i*2] = val;     // Left
            chunk[i*2+1] = val;   // Right
        }
        
        i2s_write(I2S_NUM_0, chunk, to_process * 4, &bytes_written, portMAX_DELAY);
        total_processed += to_process;
    }

    free(p);
    vTaskDelete(NULL); 
}

void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq;
        params->duration = duration_ms;
        xTaskCreate(playTaskWrapper, "PlayTask", 4096, params, 1, NULL);
        Serial.println("[Audio] Play task started");
    }
}

// ---------------- 录音功能 ----------------

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

void AppAudio::_recordTask(void *param) {
    size_t bytes_read;
    const int samples_per_chunk = 512;
    int16_t i2s_read_buff[samples_per_chunk * 2]; 

    while (isRecording) {
        i2s_read(i2s_num, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            int frames = bytes_read / 4; 

            if (record_data_len + (frames * 2) >= MAX_RECORD_SIZE) {
                Serial.println("[Audio] Buffer Full!");
                isRecording = false;
                break;
            }

            int16_t *psram_ptr = (int16_t *)(record_buffer + record_data_len);
            
            for (int i = 0; i < frames; i++) {
                psram_ptr[i] = i2s_read_buff[i * 2]; 
            }

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
    isRecording = false; 
    Serial.println("[Audio] Stopping recording...");
    delay(50); 
    uint32_t pcm_size = record_data_len - 44;
    Serial.printf("[Audio] Stop. PCM Size: %d bytes (Mono, 16k)\n", pcm_size);
    createWavHeader(record_buffer, pcm_size, 16000, 16, 1);
}

void AppAudio::playStream(WiFiClient *client, int length) {
    if (!client || length <= 0) return;

    Serial.printf("[Audio] Start Playing Stream, len: %d\n", length);
    digitalWrite(PIN_PA_EN, HIGH); 
    
    uint8_t buf[1024]; 
    size_t bytes_written;
    int remaining = length;
    
    if (remaining > 44) {
        client->readBytes(buf, 44);
        remaining -= 44;
    }

    while (remaining > 0 && client->connected()) {
        int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int len = client->readBytes(buf, to_read);
        
        if (len == 0) break;

        int samples = len / 2; 
        int16_t *pcm_samples = (int16_t*)buf;
        
        for (int i=0; i<samples; i++) {
            int16_t frame[2];
            frame[0] = pcm_samples[i];
            frame[1] = pcm_samples[i];
            i2s_write(i2s_num, frame, 4, &bytes_written, portMAX_DELAY);
        }
        
        remaining -= len;
    }
    
    uint8_t silence[128] = {0};
    i2s_write(i2s_num, silence, 128, &bytes_written, portMAX_DELAY);
    
    Serial.println("[Audio] Play Done.");
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
    header[32] = (uint8_t)(numChannels * (sampleBits / 8)); header[33] = 0; 
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
    writeReg(0x14, gain); 
}

void AppAudio::writeReg(uint8_t reg, uint8_t data) {
    // 保护：如果 I2C 没初始化成功（缺件），不执行写入，防止崩溃
    // Wire.begin() 成功后会返回 true，我们在 init 里判断过了
    // 如果要更严谨，可以加一个成员变量 _initialized 标记
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