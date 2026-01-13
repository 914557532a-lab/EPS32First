/**
 * @file App_Audio.h
 * @brief 音频控制头文件
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <WiFi.h> // 需要引用以支持 Client (或者 <Client.h>)

#define AUDIO_SAMPLE_RATE  24000 

class AppAudio {
public:
    void init();
    void setVolume(uint8_t vol);
    void setMicGain(uint8_t gain);

    // 播放提示音
    void playToneAsync(int freq, int duration_ms);

    // 录音控制
    void startRecording();
    void stopRecording();

    // [重要修改] 统一参数名为 client 和 length，类型为 Client* (兼容 WiFi 和 4G)
    void playStream(Client* client, int length);

    // 内部任务
    void _recordTask(void *param);

    uint8_t *record_buffer = NULL;       
    uint32_t record_data_len = 0;        
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; 

private:
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;
};

extern AppAudio MyAudio;

// C 语言桥接
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