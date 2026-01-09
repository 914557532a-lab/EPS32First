#include "App_Audio.h"
// #include "Pin_Config.h" // 暂时注释掉，防止引脚定义冲突，我们直接在下面写死刚才测试成功的引脚

#include "AudioTools.h"
#include "AudioBoard.h"

// --- 强制使用刚才测试成功的引脚定义 (Hardcoded for Debugging) ---
#define FORCE_I2C_SDA      47
#define FORCE_I2C_SCL      48
#define FORCE_I2S_MCLK     42
#define FORCE_I2S_BCLK     41
#define FORCE_I2S_LRCK     39
#define FORCE_I2S_DOUT     38  // 刚才测试成功的输出引脚
#define FORCE_I2S_DIN      40  // 刚才测试成功的输入引脚
#define FORCE_PA_EN        18

AppAudio MyAudio;

// 全局对象
static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s;

// 播放任务参数
struct ToneParams {
    int freq;
    int duration;
};

// 任务包装器
void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    // 使用 44100 采样率生成正弦波
    SineWaveGenerator<int16_t> sineWave(32000); 
    sineWave.begin(1, 44100, p->freq);
    
    // 计算需要的字节数
    size_t samples = (44100 * p->duration) / 1000;
    int16_t data[128];
    
    for (size_t i = 0; i < samples; i += 128) {
        // 生成数据
        for(int j=0; j<128; j++) data[j] = sineWave.readSample();
        // 写入 I2S
        i2s.write((uint8_t*)data, 256);
        // 稍微让出 CPU，防止看门狗复位
        if(i % 1024 == 0) vTaskDelay(1);
    }
    
    free(p);
    vTaskDelete(NULL); 
}

// 录音任务包装器
void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

void AppAudio::init() {
    Serial.println("\n\n=== [Audio] 正在初始化 (DEBUG模式) ===");

    // 1. 配置引脚 (使用强制定义的引脚)
    Serial.printf("[Audio] I2C Pins: SDA=%d, SCL=%d\n", FORCE_I2C_SDA, FORCE_I2C_SCL);
    my_pins.addI2C(PinFunction::CODEC, FORCE_I2C_SCL, FORCE_I2C_SDA, 0); 
    
    Serial.printf("[Audio] I2S Pins: MCLK=%d, BCLK=%d, LRCK=%d, DOUT=%d, DIN=%d\n", FORCE_I2S_MCLK, FORCE_I2S_BCLK, FORCE_I2S_LRCK, FORCE_I2S_DOUT, FORCE_I2S_DIN);
    my_pins.addI2S(PinFunction::CODEC, FORCE_I2S_MCLK, FORCE_I2S_BCLK, FORCE_I2S_LRCK, FORCE_I2S_DOUT, FORCE_I2S_DIN);
    
    Serial.printf("[Audio] PA Enable Pin: %d\n", FORCE_PA_EN);
    my_pins.addPin(PinFunction::PA, FORCE_PA_EN, PinLogic::Output);

    // 2. 配置 Codec (强制使用 44.1kHz，因为刚才测试这个是通过的)
    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_LINE1; 
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_44K; // !!! 关键：回退到 44.1k !!!

    // 3. 启动 AudioBoard
    if (board.begin(cfg)) {
        Serial.println("[Audio] ES8311 芯片初始化成功!");
    } else {
        Serial.println("[Audio] ES8311 初始化失败! 请检查 I2C 连接");
    }

    // 强制设置最大音量和增益
    board.setVolume(80);       
    board.setInputVolume(100); 

    // 4. 配置 I2S 流 (必须与 Codec 一致: 44100)
    auto config = i2s.defaultConfig(RXTX_MODE);
    config.pin_bck = FORCE_I2S_BCLK;
    config.pin_ws = FORCE_I2S_LRCK;
    config.pin_data = FORCE_I2S_DOUT;   
    config.pin_data_rx = FORCE_I2S_DIN; 
    config.pin_mck = FORCE_I2S_MCLK;    
    
    config.sample_rate = 44100; // !!! 关键：回退到 44.1k
    config.bits_per_sample = 16;
    config.channels = 1; // 单声道
    
    if (i2s.begin(config)) {
        Serial.println("[Audio] I2S 数据流启动成功!");
    } else {
        Serial.println("[Audio] I2S 启动失败!");
    }

    // 5. 内存分配
    if (psramFound()) {
        record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
        Serial.println("[Audio] 录音缓冲区已分配 (PSRAM)");
    } else {
        record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);
        Serial.println("[Audio] 录音缓冲区已分配 (SRAM)");
    }

    // --- 6. 开机自检：立即播放“滴”声 ---
    Serial.println("[Audio] 执行开机声音自检...");
    playToneAsync(880, 500); // 880Hz, 500ms
}

void AppAudio::setVolume(uint8_t vol) {
    board.setVolume(vol);
}

void AppAudio::setMicGain(uint8_t gain) {
    board.setInputVolume(gain * 10); 
}

void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq;
        params->duration = duration_ms;
        // 这里的 stack depth 改大一点，防止任务崩溃
        xTaskCreate(playTaskWrapper, "PlayTask", 1024 * 6, params, 5, NULL);
    }
}

void AppAudio::startRecording() {
    if (isRecording) return;
    if (!record_buffer) return;

    record_data_len = 44; // 预留 WAV 头
    isRecording = true;
    xTaskCreate(recordTaskWrapper, "RecTask", 1024 * 6, this, 10, &recordTaskHandle);
    Serial.println("[Audio] 开始录音...");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); // 给一点时间让任务退出
    
    uint32_t pcm_size = record_data_len - 44;
    Serial.printf("[Audio] 录音结束, 大小: %d 字节\n", pcm_size);
    
    // 生成 WAV 头 (注意这里要填 44100)
    createWavHeader(record_buffer, pcm_size, 44100, 16, 1);
}

void AppAudio::_recordTask(void *param) {
    const size_t read_size = 512;
    uint8_t temp_buf[read_size]; 

    while (isRecording) {
        size_t bytes_read = i2s.readBytes(temp_buf, read_size);
        if (bytes_read > 0) {
            // 简单增益处理 (软件放大)，如果觉得录音小可以把下面这行取消注释
            // for(int i=0; i<bytes_read; i+=2) { int16_t* s = (int16_t*)(temp_buf+i); *s = (*s) * 4; }

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
    vTaskDelete(NULL);
}

void AppAudio::playStream(WiFiClient *client, int length) {
    if (!client || length <= 0) return;
    Serial.printf("[Audio] 播放流媒体 (TTS), 长度: %d\n", length);
    
    // 注意：如果是 TTS 还是 16k，用 44.1k 播放会变快像花栗鼠
    // 这里我们先确保有声音，语速问题下一步再调
    
    uint8_t buf[512]; 
    int remaining = length;
    
    // 如果是 WAV 文件带头，跳过 44 字节
    // if (remaining > 44) { client->readBytes(buf, 44); remaining -= 44; }

    while (remaining > 0 && client->connected()) {
        int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int len = client->readBytes(buf, to_read);
        if (len == 0) break;
        i2s.write(buf, len);
        remaining -= len;
    }
    Serial.println("[Audio] 播放结束");
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

void Audio_Play_Click() { MyAudio.playToneAsync(1000, 100); }
void Audio_Record_Start() { MyAudio.startRecording(); }
void Audio_Record_Stop() { MyAudio.stopRecording(); }