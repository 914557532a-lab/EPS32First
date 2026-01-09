#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <WiFi.h>

// 移除原生的 driver/i2s.h 引用，改由 AudioTools 在 cpp 中内部处理，避免冲突
// #include <driver/i2s.h> 

class AppAudio {
public:
    void init();
    void setVolume(uint8_t vol);
    void setMicGain(uint8_t gain); // 保持接口兼容，具体实现在cpp中映射

    // 非阻塞播放一段提示音
    void playToneAsync(int freq, int duration_ms);

    // 开始录音
    void startRecording();

    // 停止录音
    void stopRecording();

    // 流式播放 (TTS)
    void playStream(WiFiClient *client, int length);

    // 内部任务处理函数 (public 以便 FreeRTOS 任务调用)
    void _playTask(void *param);
    void _recordTask(void *param);

    // --- 录音相关变量 (保持 Public 供 UI 逻辑使用) ---
    uint8_t *record_buffer = NULL;       
    uint32_t record_data_len = 0;        
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; 

private:
    // 移除旧的 writeReg/readReg，改用 AudioBoard 库接管
    // 辅助函数：生成 WAV 头
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;
};

extern AppAudio MyAudio;

// C 语言桥接接口 (保持不变)
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