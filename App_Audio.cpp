/**
 * @file App_Audio.cpp
 */
#include "App_Audio.h"
#include "Pin_Config.h" 
#include "AudioTools.h"
#include "AudioBoard.h"

AppAudio MyAudio;

static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s; 

struct ToneParams {
    int freq;
    int duration;
};

void writeSilence(int ms) {
    int bytes = (AUDIO_SAMPLE_RATE * 4 * ms) / 1000;
    uint8_t silence[256] = {0}; 
    while (bytes > 0) {
        int to_write = (bytes > 256) ? 256 : bytes;
        i2s.write(silence, to_write);
        bytes -= to_write;
    }
}

void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    digitalWrite(PIN_PA_EN, HIGH);
    delay(20);

    const int sample_rate = AUDIO_SAMPLE_RATE;
    const int amplitude = 10000; 
    int total_samples = (sample_rate * p->duration) / 1000;
    int16_t sample_buffer[256]; 
    
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            sample_buffer[2*j] = val;     
            sample_buffer[2*j+1] = val;   
        }
        i2s.write((uint8_t*)sample_buffer, batch * 4);
        if(i % 1024 == 0) delay(1);
    }
    writeSilence(20);
    digitalWrite(PIN_PA_EN, LOW);
    free(p);
    vTaskDelete(NULL); 
}

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); 
    vTaskDelete(NULL);
}

// --- Class Implementation ---

void AppAudio::init() {
    Serial.println("[Audio] Init...");
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW); 

    my_pins.addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, 0); 
    my_pins.addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN);
    my_pins.addPin(PinFunction::PA, PIN_PA_EN, PinLogic::Output);

    CodecConfig cfg;
    cfg.input_device = ADC_INPUT_ALL; 
    cfg.output_device = DAC_OUTPUT_ALL;
    cfg.i2s.bits = BIT_LENGTH_16BITS;
    cfg.i2s.rate = RATE_16K; 
    cfg.i2s.fmt = I2S_NORMAL; 

    if (board.begin(cfg)) Serial.println("[Audio] Codec OK");
    else Serial.println("[Audio] Codec FAIL");

    board.setVolume(35);       
    board.setInputVolume(0); 

    auto config = i2s.defaultConfig(RXTX_MODE); 
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    config.sample_rate = AUDIO_SAMPLE_RATE; 
    config.bits_per_sample = 16;
    config.channels = 2;       
    config.use_apll = true;    
    
    if (i2s.begin(config)) Serial.println("[Audio] I2S OK");
    else Serial.println("[Audio] I2S FAIL");

    if (psramFound()) record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    else record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);
    
    playToneAsync(1000, 200);
}

void AppAudio::setVolume(uint8_t vol) { board.setVolume(vol); }
void AppAudio::setMicGain(uint8_t gain) { board.setInputVolume(gain); }

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
    board.setInputVolume(85); 
    record_data_len = 44; 
    isRecording = true;
    xTaskCreate(recordTaskWrapper, "RecTask", 8192, this, 10, &recordTaskHandle);
    Serial.println("[Audio] Start Rec");
}

void AppAudio::stopRecording() {
    isRecording = false; 
    delay(100); 
    board.setInputVolume(0); 
    uint32_t pcm_size = record_data_len - 44;
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
            } else isRecording = false; 
        } else vTaskDelay(1);
    }
}

// [核心修复] 参数类型为 Client*，参数名为 client，与头文件一致
void AppAudio::playStream(Client* client, int length) {
    // 检查指针是否为空
    if (!client || length <= 0) return;

    Serial.printf("[Audio] Playing Stream: %d bytes\n", length);
    digitalWrite(PIN_PA_EN, HIGH); 
    delay(20); 
    writeSilence(20);

    const int buff_size = 2048; 
    uint8_t buff[buff_size]; 
    int16_t stereo_buff[buff_size]; 

    int remaining = length;
    
    // 使用 client->connected() 和 client->available()
    while (remaining > 0 && client->connected()) {
        int max_read = (remaining > (buff_size / 2)) ? (buff_size / 2) : remaining;
        
        int bytesIn = 0;
        unsigned long startWait = millis();
        while (bytesIn < max_read && millis() - startWait < 1000) {
             if (client->available()) {
                 bytesIn += client->read(buff + bytesIn, max_read - bytesIn);
             } else {
                 delay(1);
             }
        }
        
        if (bytesIn == 0) break; 

        int sample_count = bytesIn / 2; 
        int16_t *pcm_samples = (int16_t*)buff;

        for (int i = 0; i < sample_count; i++) {
            int16_t val = pcm_samples[i];
            stereo_buff[i*2]     = val; 
            stereo_buff[i*2 + 1] = val; 
        }

        i2s.write((uint8_t*)stereo_buff, sample_count * 4);
        remaining -= bytesIn;
    }
    
    writeSilence(50);
    digitalWrite(PIN_PA_EN, LOW); 
    Serial.println("[Audio] Stream End");
}

// 替换 App_Audio.cpp 中的 playChunk 函数
void AppAudio::playChunk(uint8_t* data, size_t len) {
    if (data == NULL || len == 0) return;

    // Serial.printf("[Audio] PlayChunk: %d bytes\n", len); // 嫌刷屏太快可以注释掉

    // ================================================================
    // [重要修改] 删除了这里的 功放开启 和 delay
    // 流式播放时，不能每播放一小段就开关一次功放，否则声音全是断的
    // ================================================================

    // 2. 单声道转立体声处理 (保持不变)
    size_t sample_count = len / 2; 
    const int BATCH_SAMPLES = 256; 
    int16_t stereo_batch[BATCH_SAMPLES * 2]; 

    int16_t *pcm_in = (int16_t*)data; 

    for (size_t i = 0; i < sample_count; i += BATCH_SAMPLES) {
        size_t remain = sample_count - i;
        size_t current_batch_size = (remain > BATCH_SAMPLES) ? BATCH_SAMPLES : remain;

        for (size_t j = 0; j < current_batch_size; j++) {
            int16_t val = pcm_in[i + j]; 
            stereo_batch[j * 2]     = val; 
            stereo_batch[j * 2 + 1] = val; 
        }

        i2s.write((uint8_t*)stereo_batch, current_batch_size * 4);
    }

    // ================================================================
    // [重要修改] 删除了这里的 功放关闭
    // ================================================================
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