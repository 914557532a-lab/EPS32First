/**
 * @file App_Audio.cpp
 * @brief 音频实现 - 环形缓冲流式播放 (低延迟 + 高抗抖动)
 */
#include "App_Audio.h"
#include "Pin_Config.h" 
#include "AudioTools.h"
#include "AudioBoard.h"

AppAudio MyAudio;

static DriverPins my_pins;
static AudioBoard board(AudioDriverES8311, my_pins);
static I2SStream i2s; 

// =========================================================
// [核心组件] PSRAM 环形缓冲区 (Ring Buffer)
// =========================================================
// 定义 256KB 的环形缓冲区，足够抗网络抖动
#define STREAM_BUFFER_SIZE (256 * 1024)

uint8_t* g_ringBuf = NULL;    // 指向 PSRAM
volatile uint32_t g_head = 0;  // 写入位置
volatile uint32_t g_tail = 0;  // 读取位置
volatile uint32_t g_count = 0; // 当前有效数据量
SemaphoreHandle_t g_mutex = NULL; // 互斥锁

// 状态标志
volatile bool g_isStreaming = false; 
volatile bool g_streamEnding = false;

struct ToneParams { int freq; int duration; };

// 辅助函数：静音 (防止爆音)
void writeSilence(int ms) {
    int bytes = (AUDIO_SAMPLE_RATE * 4 * ms) / 1000;
    uint8_t silence[128] = {0};
    while (bytes > 0) {
        int to_write = (bytes > 128) ? 128 : bytes;
        i2s.write(silence, to_write);
        bytes -= to_write;
        // 只有大量静音时才延时，避免影响正常播放流
        if (bytes > 1024) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 播放提示音任务
void playTaskWrapper(void *param) {
    ToneParams *p = (ToneParams*)param;
    digitalWrite(PIN_PA_EN, HIGH); delay(20);
    const int sample_rate = AUDIO_SAMPLE_RATE; const int amplitude = 10000; 
    int total_samples = (sample_rate * p->duration) / 1000;
    int16_t sample_buffer[256]; 
    for (int i = 0; i < total_samples; i += 128) {
        int batch = (total_samples - i) > 128 ? 128 : (total_samples - i);
        for (int j = 0; j < batch; j++) {
            int16_t val = (int16_t)(amplitude * sin(2 * PI * p->freq * (i + j) / sample_rate));
            sample_buffer[2*j] = val; sample_buffer[2*j+1] = val;   
        }
        i2s.write((uint8_t*)sample_buffer, batch * 4);
        if(i % 512 == 0) vTaskDelay(pdMS_TO_TICKS(1)); 
    }
    writeSilence(20); digitalWrite(PIN_PA_EN, LOW);
    free(p); vTaskDelete(NULL); 
}

void recordTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_recordTask(NULL); vTaskDelete(NULL);
}

// [新增] 专门的流播放任务包装器
void streamPlayTaskWrapper(void *param) {
    AppAudio *audio = (AppAudio *)param;
    audio->_playTask(NULL);
    vTaskDelete(NULL);
}

// --- Init ---

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
    cfg.i2s.rate = RATE_24K;  // 24kHz
    cfg.i2s.fmt = I2S_NORMAL; 

    if (board.begin(cfg)) Serial.println("[Audio] Codec OK");
    else Serial.println("[Audio] Codec FAIL");

    board.setVolume(25); board.setInputVolume(0); 

    auto config = i2s.defaultConfig(RXTX_MODE); 
    config.pin_bck = PIN_I2S_BCLK;
    config.pin_ws = PIN_I2S_LRCK;
    config.pin_data = PIN_I2S_DOUT;
    config.pin_data_rx = PIN_I2S_DIN;
    config.pin_mck = PIN_I2S_MCLK; 
    config.sample_rate = AUDIO_SAMPLE_RATE; // 24000
    config.bits_per_sample = 16;
    config.channels = 2;       
    config.use_apll = true;    
    
    if (i2s.begin(config)) Serial.println("[Audio] I2S OK");
    
    // 初始化录音缓冲
    if (psramFound()) record_buffer = (uint8_t *)ps_malloc(MAX_RECORD_SIZE);
    else record_buffer = (uint8_t *)malloc(MAX_RECORD_SIZE);

    // [新增] 初始化流播放环形缓冲区
    if (psramFound()) g_ringBuf = (uint8_t *)ps_malloc(STREAM_BUFFER_SIZE);
    else g_ringBuf = (uint8_t *)malloc(STREAM_BUFFER_SIZE);

    g_mutex = xSemaphoreCreateMutex();

    // [新增] 启动后台播放守护任务
    xTaskCreate(streamPlayTaskWrapper, "StreamPlay", 4096, this, 5, NULL);

    playToneAsync(1000, 200);
}

void AppAudio::setVolume(uint8_t vol) { board.setVolume(vol); }
void AppAudio::setMicGain(uint8_t gain) { board.setInputVolume(gain); }

// =========================================================
// [核心逻辑] 流式接口实现
// =========================================================

void AppAudio::streamStart() {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_head = 0;
    g_tail = 0;
    g_count = 0;
    g_isStreaming = true; 
    g_streamEnding = false;
    xSemaphoreGive(g_mutex);
    Serial.println("[Audio] Stream Start. Buffer Cleared.");
}

void AppAudio::streamWrite(uint8_t* data, size_t len) {
    if (!g_ringBuf || len == 0) return;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    
    size_t spaceFree = STREAM_BUFFER_SIZE - g_count;
    if (len > spaceFree) {
        // 缓冲区满，这是异常情况，一般不会发生
        len = spaceFree; 
    }

    // 写入环形缓冲区
    for (size_t i = 0; i < len; i++) {
        g_ringBuf[g_head] = data[i];
        g_head = (g_head + 1) % STREAM_BUFFER_SIZE;
    }
    g_count += len;
    
    xSemaphoreGive(g_mutex);
}

void AppAudio::streamEnd() {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_streamEnding = true; // 告诉播放任务：没有新数据了
    xSemaphoreGive(g_mutex);
    Serial.println("[Audio] Stream End Marked.");
}

// [最关键的任务] 后台播放任务
void AppAudio::_playTask(void *param) {
    // 预缓冲阈值：
    // 24kHz * 2通道 * 2字节 = 96000 bytes/s
    // 设为 24000 bytes，约为 0.25秒延迟。既流畅又快。
    const uint32_t START_THRESHOLD = 24000; 
    
    // [修复] 重命名变量，避免与 AudioTools 库的宏定义冲突
    const size_t PLAY_CHUNK_SIZE = 512; 
    uint8_t tempBuf[PLAY_CHUNK_SIZE];
    
    // 立体声转换缓冲
    int16_t stereoBuf[PLAY_CHUNK_SIZE]; // 512字节单声道 -> 1024字节立体声

    bool isPlaying = false;

    for (;;) {
        // 如果未处于流模式，休息
        if (!g_isStreaming) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t bytesAvailable = 0;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        bytesAvailable = g_count;
        xSemaphoreGive(g_mutex);

        // 状态机逻辑
        if (!isPlaying) {
            // 状态：缓冲中
            // 开始播放条件：数据够了 OR 流已经结束了(短回复)
            if (bytesAvailable > START_THRESHOLD || (g_streamEnding && bytesAvailable > 0)) {
                Serial.printf("[Audio] Playing... (Buffered: %d)\n", bytesAvailable);
                digitalWrite(PIN_PA_EN, HIGH); 
                vTaskDelay(20); // 开启功放
                isPlaying = true;
            } else {
                // 数据不够，继续等待
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // 状态：播放中
        if (bytesAvailable > 0) {
            // 1. 从环形缓冲取数据
            size_t readLen = (bytesAvailable > PLAY_CHUNK_SIZE) ? PLAY_CHUNK_SIZE : bytesAvailable;
            
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            for (size_t i = 0; i < readLen; i++) {
                tempBuf[i] = g_ringBuf[g_tail];
                g_tail = (g_tail + 1) % STREAM_BUFFER_SIZE;
            }
            g_count -= readLen;
            xSemaphoreGive(g_mutex);

            // 2. 单声道转立体声
            size_t sampleCount = readLen / 2; // 16bit samples
            int16_t* pIn = (int16_t*)tempBuf;
            for (size_t i = 0; i < sampleCount; i++) {
                stereoBuf[i*2] = pIn[i];
                stereoBuf[i*2+1] = pIn[i];
            }

            // 3. 写入 I2S (这里会阻塞，直到 DMA 接受数据)
            // 独立任务阻塞不影响网络下载
            i2s.write((uint8_t*)stereoBuf, sampleCount * 4);

        } else {
            // 缓冲区空了
            if (g_streamEnding) {
                // 彻底播完了
                Serial.println("[Audio] Playback Finished.");
                writeSilence(50); // 消除尾音
                digitalWrite(PIN_PA_EN, LOW); // 关功放
                
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_isStreaming = false;
                isPlaying = false;
                g_streamEnding = false;
                xSemaphoreGive(g_mutex);
            } else {
                // 流还没结束，但数据断了 -> 网络卡顿 (Buffering...)
                // 保持功放开启，等待数据
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }
}

// 录音任务 (保持原样)
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

// 兼容旧接口 (PlayStream / PlayChunk) - 简单封装
void AppAudio::playStream(Client* client, int length) {
    // 流式版本暂不使用此阻塞接口
}

// 兼容 4G 非流式调用
void AppAudio::playChunk(uint8_t* data, size_t len) {
    // 为了兼容性，这里只是简单播放，建议4G也改用 streamWrite
    // 但如果必须用，保持之前的实现：
    if (data == NULL || len == 0) return;
    size_t sample_count = len / 2; 
    const int BATCH_SAMPLES = 256; 
    int16_t stereo_batch[BATCH_SAMPLES * 2]; 
    int16_t *pcm_in = (int16_t*)data; 
    for (size_t i = 0; i < sample_count; i += BATCH_SAMPLES) {
        size_t remain = sample_count - i;
        size_t current_batch_size = (remain > BATCH_SAMPLES) ? BATCH_SAMPLES : remain;
        for (size_t j = 0; j < current_batch_size; j++) {
            int16_t val = pcm_in[i + j]; 
            stereo_batch[j * 2] = val; stereo_batch[j * 2 + 1] = val; 
        }
        i2s.write((uint8_t*)stereo_batch, current_batch_size * 4);
        if (i % 512 == 0) vTaskDelay(pdMS_TO_TICKS(1)); 
    }
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

// 提示音接口
void AppAudio::playToneAsync(int freq, int duration_ms) {
    ToneParams *params = (ToneParams*)malloc(sizeof(ToneParams));
    if(params) {
        params->freq = freq; params->duration = duration_ms;
        xTaskCreate(playTaskWrapper, "PlayTask", 4096, params, 2, NULL);
    }
}

// C 接口
void Audio_Play_Click() { MyAudio.playToneAsync(1000, 100); }
void Audio_Record_Start() { MyAudio.startRecording(); }
void Audio_Record_Stop() { MyAudio.stopRecording(); }