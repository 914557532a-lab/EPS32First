#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <Arduino.h>
#include <driver/i2s.h>

#define ES8311_ADDR     0x18

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

    // 内部任务处理函数
    void _playTask(void *param);
    void _recordTask(void *param);

    // --- 修复点：将这些变量移到 public 区域，以便外部 (UI Logic) 可以读取 ---
    // --- 新增：录音相关变量 ---
    uint8_t *record_buffer = NULL;       // 录音缓冲区指针
    uint32_t record_data_len = 0;        // 当前已录制的数据长度
    const uint32_t MAX_RECORD_SIZE = 1024 * 512; // 定义最大录音大小

private:
    void writeReg(uint8_t reg, uint8_t data);
    uint8_t readReg(uint8_t reg);

    // --- 新增：WAV 头部生成辅助函数声明 ---
    void createWavHeader(uint8_t *header, uint32_t totalDataLen, uint32_t sampleRate, uint8_t sampleBits, uint8_t numChannels);

    const i2s_port_t i2s_num = I2S_NUM_0;
    
    TaskHandle_t recordTaskHandle = NULL;
    volatile bool isRecording = false;

    // (原先在这里的变量已移动到 public)
};

extern AppAudio MyAudio;

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