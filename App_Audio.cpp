/**
 * @file App_Audio.cpp
 * @brief 音频实现 - 环形缓冲流式播放 (修复宏冲突与阻塞写入)
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
// 256KB 缓冲区
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
        if (bytes > 4096) vTaskDelay(pdMS_TO_TICKS(1));
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

    board.setVolume(25); // 默认音量
    board.setInputVolume(0); 

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
    Serial.println("[Audio] Stream Start.");
}

// [修复] 阻塞式写入，防止数据丢失
void AppAudio::streamWrite(uint8_t* data, size_t len) {
    if (!g_ringBuf || len == 0) return;

    size_t written = 0;
    
    // 循环直到所有数据都写入缓冲区
    while (written < len) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        
        size_t spaceFree = STREAM_BUFFER_SIZE - g_count;
        
        if (spaceFree > 0) {
            // 本次能写的量
            size_t toWrite = (len - written) > spaceFree ? spaceFree : (len - written);
            
            // 环形写入
            for (size_t i = 0; i < toWrite; i++) {
                g_ringBuf[g_head] = data[written++];
                g_head = (g_head + 1) % STREAM_BUFFER_SIZE;
            }
            g_count += toWrite;
            
            xSemaphoreGive(g_mutex);
        } else {
            // 缓冲区满了，解锁并等待播放器消费
            xSemaphoreGive(g_mutex);
            // 等待时间根据写入压力调整，2ms 比较合适
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

void AppAudio::streamEnd() {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_streamEnding = true; // 告诉播放任务：没有新数据了
    xSemaphoreGive(g_mutex);
    Serial.println("[Audio] Stream End Marked.");
}

// [最关键的任务] 后台播放任务
void AppAudio::_playTask(void *param) {
    // 预缓冲阈值
    const uint32_t START_THRESHOLD = 19200; 
    
    // [修复] 重命名为 PLAY_CHUNK_SIZE，避免与 AudioTools 库的 CHUNK_SIZE 宏冲突
    const size_t PLAY_CHUNK_SIZE = 256; 
    uint8_t tempBuf[PLAY_CHUNK_SIZE];
    
    // 立体声转换缓冲 (int16_t 数组，大小足够容纳双通道样本)
    // 256字节单声道数据 = 128个样本。立体声需要 128*2 = 256 个样本。
    // 所以 int16_t[256] 刚好够用。
    int16_t stereoBuf[PLAY_CHUNK_SIZE]; 
    
    bool isPlaying = false;

    for (;;) {
        // 如果未处于流模式，休息
        if (!g_isStreaming) {
            if (isPlaying) {
                // 刚结束一次播放
                writeSilence(50);
                digitalWrite(PIN_PA_EN, LOW);
                isPlaying = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t bytesAvailable = 0;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        bytesAvailable = g_count;
        xSemaphoreGive(g_mutex);

        // 1. 状态：缓冲中 (Buffering)
        if (!isPlaying) {
            // 开始播放条件：数据够了 OR 流已经标记结束(短语音)
            if (bytesAvailable > START_THRESHOLD || (g_streamEnding && bytesAvailable > 0)) {
                Serial.printf("[Audio] Playing... (Buffered: %d)\n", bytesAvailable);
                digitalWrite(PIN_PA_EN, HIGH); 
                vTaskDelay(20); // 等待功放开启稳定
                isPlaying = true;
            } else {
                // 数据不够，继续等待写入
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // 2. 状态：播放中 (Playing)
        if (bytesAvailable > 0) {
            // (A) 从环形缓冲取数据
            size_t readLen = (bytesAvailable > PLAY_CHUNK_SIZE) ? PLAY_CHUNK_SIZE : bytesAvailable;
            
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            for (size_t i = 0; i < readLen; i++) {
                tempBuf[i] = g_ringBuf[g_tail];
                g_tail = (g_tail + 1) % STREAM_BUFFER_SIZE;
            }
            g_count -= readLen;
            xSemaphoreGive(g_mutex);

            // (B) 单声道 PCM (16bit) 转 立体声 I2S (16bit * 2)
            // tempBuf 是小端序的 PCM 数据
            size_t sampleCount = readLen / 2; // 样本数
            int16_t* pIn = (int16_t*)tempBuf;
            
            // 构造立体声数据
            // I2S 需要: L-Sample(2B), R-Sample(2B), L-Sample...
            for (size_t i = 0; i < sampleCount; i++) {
                int16_t val = pIn[i];
                stereoBuf[i*2]     = val; // Left
                stereoBuf[i*2 + 1] = val; // Right
            }

            // (C) 写入 I2S (阻塞直到 DMA 接受)
            // write 接收的是字节长度：样本数 * 2通道 * 2字节 = sampleCount * 4
            i2s.write((uint8_t*)stereoBuf, sampleCount * 4);

        } else {
            // 缓冲区空了
            if (g_streamEnding) {
                // 数据读完了，且流标记结束 -> 彻底结束
                Serial.println("[Audio] Playback Finished.");
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_isStreaming = false; // 回到待机状态
                xSemaphoreGive(g_mutex);
            } else {
                // 流没结束，但缓冲区空了 -> 网络卡顿 (Underrun)
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
    createWavHeader(record_buffer, pcm_size, AUDIO_SAMPLE_RATE, 16, 2); // 录音是双通道
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

// 兼容旧接口
void AppAudio::playStream(Client* client, int length) {}
void AppAudio::playChunk(uint8_t* data, size_t len) {}

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