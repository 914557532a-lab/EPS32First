#include "App_Audio.h"
#include "Pin_Config.h"

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

// 任务包装函数
void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    
    // --- [修改 1] 播放也要匹配立体声设置 ---
    const int sample_rate = 16000;
    const int channels = 2; // 双声道
    const int amplitude = 10000; // 稍微调大一点振幅
    
    int total_samples = (sample_rate * p->duration) / 1000;
    // 缓冲区大小 = 采样点 * 通道数 * 2字节(int16)
    // 128帧 * 2通道 = 256个int16
    int16_t sample_buffer[256]; 
    
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            // 双声道填充：左声道 = val, 右声道 = val
            sample_buffer[2*j] = val;     
            sample_buffer[2*j+1] = val;   
        }
        // 写入字节数 = batch * 2通道 * 2字节
        i2s.write((uint8_t*)sample_buffer, batch * 2 * 2);
        
        // 稍微延时防止看门狗
        if(i % 1024 == 0) vTaskDelay(1);
    }

    // 播放结束写静音
    uint8_t silence[128] = {0};
    i2s.write(silence, 128);

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
    Serial.println("[Audio] Init Start (Stereo Mode)...");

    // 1. 再次强制拉高 PA_EN，确保功放有电
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, HIGH);

    // 2. 配置 DriverPins
    my_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0); 
    my_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    my_pins.addPin(PinFunction::PA, PIN_PA_EN, PinLogic::Output);

    // 3. 配置 ES8311 Codec 参数
    CodecConfig cfg;
    
    // --- [修改 2] 尝试开启所有麦克风输入 ---
    // 如果不知道麦克风接在 Line1 还是 Line2，先开 ALL
    // 如果声音有杂音，后面再改成 ADC_INPUT_LINE1 或 ADC_INPUT_LINE2 逐个测试
    cfg.input_device = ADC_INPUT_ALL; 
    
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_16K; 

    // 4. 初始化芯片
    if (board.begin(cfg)) {
        Serial.println("[Audio] ES8311 Init OK!");
    } else {
        Serial.println("[Audio] ES8311 Init FAILED! (Check I2C)");
    }

    // 5. 设置音量和增益
    board.setVolume(70);       
    board.setInputVolume(100); // 录音增益拉满

    // 6. 配置 I2S 数据流
    auto config = i2s.defaultConfig(RXTX_MODE); 
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    
    config.sample_rate = 16000;
    config.bits_per_sample = 16;
    
    // --- [修改 3] 强制改为双声道 (Stereo) ---
    // ES8311 的 ADC 和 DAC 时序依赖标准的立体声 I2S 时钟
    config.channels = 2; 
    
    if (i2s.begin(config)) {
        Serial.println("[Audio] I2S Stream Started (16kHz, Stereo)");
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
        // 栈空间给大一点
        xTaskCreate(playTaskWrapper, "PlayTask", 8192, params, 2, NULL);
    }
}

void AppAudio::startRecording() {
    if (isRecording) return;
    if (!record_buffer) return;

    record_data_len = 44; // 预留44字节WAV头
    isRecording = true;
    
    xTaskCreate(recordTaskWrapper, "RecTask", 8192, this, 10, &recordTaskHandle);
    Serial.println("[Audio] Start Recording (Stereo)...");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); 
    
    uint32_t pcm_size = record_data_len - 44;
    Serial.printf("[Audio] Record Stop. Raw Size: %d bytes\n", pcm_size);
    
    // --- [修改 4] 生成 WAV 头时指定 2 通道 ---
    // createWavHeader(buffer, size, rate, bits, channels)
    createWavHeader(record_buffer, pcm_size, 16000, 16, 2);
}

void AppAudio::_recordTask(void *param) {
    // 因为是立体声，每次读的数据量建议翻倍
    const size_t read_size = 1024; 
    uint8_t temp_buf[read_size]; 

    while (isRecording) {
        // I2S 读取是阻塞的，如果没有数据会卡在这里，或者返回0
        size_t bytes_read = i2s.readBytes(temp_buf, read_size);

        if (bytes_read > 0) {
            if (record_data_len + bytes_read < MAX_RECORD_SIZE) {
                memcpy(record_buffer + record_data_len, temp_buf, bytes_read);
                record_data_len += bytes_read;
            } else {
                Serial.println("[Audio] Buffer Full!");
                isRecording = false; 
            }
        } else {
            vTaskDelay(1);
        }
    }
}

void AppAudio::playStream(WiFiClient *client, int length) {
    if (!client || length <= 0) return;

    Serial.printf("[Audio] Playing Stream: %d bytes\n", length);
    
    uint8_t buf[1024]; 
    int remaining = length;
    
    while (remaining > 0 && client->connected()) {
        int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int len = client->readBytes(buf, to_read);
        if (len == 0) break;

        // 注意：如果 TTS 返回的是单声道数据，直接写进立体声 I2S 
        // 声音会变得非常快（2倍速）。
        // 暂时先让它响起来，后面如果语速不对，需要做软件单转双处理。
        i2s.write(buf, len);
        
        remaining -= len;
    }
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
    header[22] = numChannels; header[23] = 0; // 通道数写入头文件
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