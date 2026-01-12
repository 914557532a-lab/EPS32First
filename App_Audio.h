/**
 * @file App_Audio.h
 * @brief 音频控制头文件 - 集成 APLL 和 滤波优化
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>

// 移除原生的 driver/i2s.h 引用，改由 AudioTools 内部处理
// #include <driver/i2s.h> 

// ================= 音频核心配置 =================
// 原始 TTS 数据通常是 16k 或 32k。
// 设置为 24000 可以适当放慢语速，压低声调，听起来更自然。
// 同时用于录音采样率。
#define AUDIO_SAMPLE_RATE  24000 

class AppAudio {
public:
    void init();
    void setVolume(uint8_t vol);
    void setMicGain(uint8_t gain);

    // 非阻塞播放一段提示音
    void playToneAsync(int freq, int duration_ms);

    // 开始录音
    void startRecording();

    // 停止录音
    void stopRecording();

    // 流式播放 (TTS) - 经过优化
    void playStream(WiFiClient *client, int length);

    // 内部任务处理函数 (public 以便 FreeRTOS 任务调用)
    void _recordTask(void *param);

    // --- 录音相关变量 ---
    uint8_t *record_buffer = NULL;       
    uint32_t record_data_len = 0;        
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; 

private:
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;
};

extern AppAudio MyAudio;

// C 语言桥接接口
#ifdef __cplusplus
extern "C" {
#endif
void Audio_Play_Click();
void Audio_Record_Start();
void Audio_Record_Stop();
#ifdef __cplusplus
}
#endif

#endif // APP_AUDIO_H