#include "App_Audio.h"
#include <Wire.h>
#include <math.h>
#include "Pin_Config.h" 
#include "driver/gpio.h"

AppAudio MyAudio;

// ES8311 麦克风增益枚举 (对应寄存器 0x16)
typedef enum {
    ES8311MIC_GAIN_0DB = 0,
    ES8311MIC_GAIN_6DB = 1,
    ES8311MIC_GAIN_12DB = 2,
    ES8311MIC_GAIN_18DB = 3,
    ES8311MIC_GAIN_24DB = 4,
    ES8311MIC_GAIN_30DB = 5,
    ES8311MIC_GAIN_36DB = 6,
    ES8311MIC_GAIN_42DB = 7,
} es8311_mic_gain_t;

// --- 寄存器定义 (同步自 es8311.h) ---
#define ES8311_RESET_REG00              0x00
#define ES8311_CLK_MANAGER_REG01        0x01
#define ES8311_CLK_MANAGER_REG02        0x02
#define ES8311_CLK_MANAGER_REG03        0x03
#define ES8311_CLK_MANAGER_REG04        0x04
#define ES8311_CLK_MANAGER_REG05        0x05
#define ES8311_CLK_MANAGER_REG06        0x06
#define ES8311_CLK_MANAGER_REG07        0x07
#define ES8311_CLK_MANAGER_REG08        0x08
#define ES8311_SDPIN_REG09              0x09
#define ES8311_SDPOUT_REG0A             0x0A
#define ES8311_SYSTEM_REG0D             0x0D
#define ES8311_SYSTEM_REG0E             0x0E
#define ES8311_SYSTEM_REG12             0x12
#define ES8311_SYSTEM_REG13             0x13
#define ES8311_SYSTEM_REG14             0x14
#define ES8311_ADC_REG15                0x15
#define ES8311_ADC_REG16                0x16
#define ES8311_ADC_REG17                0x17
#define ES8311_DAC_REG31                0x31
#define ES8311_DAC_REG32                0x32
#define ES8311_DAC_REG37                0x37
#define ES8311_GP_REG45                 0x45

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



void AppAudio::init() {
    Serial.println("[Audio] Init with ES8311 Official Logic...");

    if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.println("[Audio] ERROR: I2C Init Failed!");
        return; 
    }
    
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW); // 先关闭功放

    // 强制复位 JTAG 引脚 (GPIO 39-42) 确保 I2S 信号正常
    gpio_reset_pin((gpio_num_t)PIN_I2S_MCLK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_BCLK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_LRCK);
    gpio_reset_pin((gpio_num_t)PIN_I2S_DOUT);
    gpio_reset_pin((gpio_num_t)PIN_I2S_DIN);

    // I2S 配置: 16kHz, 16bit, Standard I2S
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = true, // 使用 APLL 获得更精准的时钟
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0   // 自动生成 MCLK (通常是 256 * 16k = 4.096MHz)
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

    // --- ES8311 核心初始化序列 (参考 es8311.c) ---
    Serial.println("[Audio] Configuring ES8311 Registers...");
    
    // 1. 初始时钟配置
    writeReg(ES8311_CLK_MANAGER_REG01, 0x30);
    writeReg(ES8311_CLK_MANAGER_REG02, 0x00);
    writeReg(ES8311_CLK_MANAGER_REG03, 0x10);
    writeReg(ES8311_ADC_REG16, 0x24); // 初始麦克风增益
    writeReg(ES8311_CLK_MANAGER_REG04, 0x10);
    writeReg(ES8311_CLK_MANAGER_REG05, 0x00);
    writeReg(0x0B, 0x00);
    writeReg(0x0C, 0x00);
    writeReg(0x10, 0x1F);
    writeReg(0x11, 0x7F);
    
    // 2. 软件复位
    writeReg(ES8311_RESET_REG00, 0x80); 
    delay(5);
    writeReg(ES8311_RESET_REG00, 0x00); // 退出复位 (Slave模式, bit6=0)

    // 3. 针对 16kHz / MCLK=256Fs 的时钟精调 (参考 coeff_div 表)
    // 对于 4.096MHz MCLK: pre_div=1, multi=1, bclk_div=4
    writeReg(ES8311_CLK_MANAGER_REG01, 0x3F); 
    writeReg(ES8311_CLK_MANAGER_REG02, 0x00); // pre_div=1, multi=1
    writeReg(ES8311_CLK_MANAGER_REG05, 0x00); // adc/dac div=1
    writeReg(ES8311_CLK_MANAGER_REG03, 0x10); // fs_mode=0, adc_osr=16
    writeReg(ES8311_CLK_MANAGER_REG04, 0x10); // dac_osr=16
    writeReg(ES8311_CLK_MANAGER_REG06, 0x03); // bclk_div=4 (4-1=3)
    writeReg(ES8311_CLK_MANAGER_REG07, 0x00); // lrck_h
    writeReg(ES8311_CLK_MANAGER_REG08, 0xFF); // lrck_l

    // 4. 接口格式设置 (Standard I2S, 16bit)
    writeReg(ES8311_SDPIN_REG09, 0x0C);  // DAC 16bit, I2S
    writeReg(ES8311_SDPOUT_REG0A, 0x0C); // ADC 16bit, I2S

    // 5. 电源管理与启动
    writeReg(ES8311_SYSTEM_REG13, 0x10);
    writeReg(0x1B, 0x0A);
    writeReg(0x1C, 0x6A);
    
    // 启动序列 (参考 es8311_start)
    writeReg(ES8311_ADC_REG17, 0xBF);
    writeReg(ES8311_SYSTEM_REG0E, 0x02);
    writeReg(ES8311_SYSTEM_REG12, 0x00);
    writeReg(ES8311_SYSTEM_REG14, 0x1A);
    writeReg(ES8311_SYSTEM_REG0D, 0x01);
    writeReg(ES8311_ADC_REG15, 0x40);
    writeReg(ES8311_DAC_REG32, 0xBF); // 默认音量
    writeReg(ES8311_DAC_REG37, 0x48);
    writeReg(ES8311_GP_REG45, 0x00);

    setVolume(75);
    setMicGain(ES8311MIC_GAIN_30DB); // 提升麦克风增益解决录音空的问题
    
    digitalWrite(PIN_PA_EN, HIGH); // 开启功放
    
    // 申请 PSRAM
    record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    if (!record_buffer) record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);
    
    Serial.println("[Audio] Init Done with Official Logic.");
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

void AppAudio::setVolume(uint8_t volume) {
    if (volume > 100) volume = 100;
    // 使用官方驱动的对数映射算法，让音量调节平滑
    int vol_reg = 0;
    if (volume != 0) {
        vol_reg = (int)(255.0 * log10(9.0 * volume / 100.0 + 1.0) / log10(10.0));
    }
    writeReg(ES8311_DAC_REG32, vol_reg);
}

void AppAudio::setMicGain(uint8_t gain_db) {
    // 修正：官方驱动中，录音增益主要由 REG 0x16 控制
    // 建议传入枚举值如 ES8311MIC_GAIN_30DB (0x05)
    writeReg(ES8311_ADC_REG16, gain_db); 
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