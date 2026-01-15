/**
 * @file App_Audio.h
 * @brief 音频控制头文件 - 流式低延迟版 (Pipeline Streaming)
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>

// [配置] 采样率 24kHz (配合服务端)
#define AUDIO_SAMPLE_RATE  24000 

class AppAudio {
public:
    void init();
    void setVolume(uint8_t vol);
    void setMicGain(uint8_t gain); // 添加漏掉的 setMicGain
    
    // 播放提示音 (异步)
    void playToneAsync(int freq, int duration_ms);

    // 录音控制
    void startRecording();
    void stopRecording();
    void _recordTask(void *param);

    // --- [核心新增] 流式播放接口 ---
    // 1. 开始流任务 (重置缓冲区，准备接收)
    void streamStart();
    // 2. 写入数据到环形缓冲 (由 Server 任务调用)
    void streamWrite(uint8_t* data, size_t len);
    // 3. 标记流结束 (通知播放任务：数据发完了，播完剩下的就结束)
    void streamEnd();
    
    // 内部后台播放任务
    void _playTask(void *param);

    // 兼容旧逻辑的直接播放接口 (供 4G 模式或非流式场景使用)
    void playChunk(uint8_t* data, size_t len);
    
    // 兼容旧接口 (如果 Server 还需要用)
    void playStream(Client* client, int length);

    // 录音相关变量
    uint8_t *record_buffer = NULL;       
    uint32_t record_data_len = 0;        
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; // 512KB

private:
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;
};

extern AppAudio MyAudio;

// C 接口
#ifdef __cplusplus
extern "C" {
#endif
void Audio_Play_Click();
void Audio_Record_Start();
void Audio_Record_Stop();
#ifdef __cplusplus
}
#endif

#endif