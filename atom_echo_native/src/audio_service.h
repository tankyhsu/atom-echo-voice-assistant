#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <cstdint>
#include <functional>

#include "audio_codec.h"

// Opus frame: 60ms at 16kHz = 960 samples
#define OPUS_FRAME_DURATION_MS  60
#define OPUS_ENCODE_SAMPLE_RATE 16000
#define OPUS_FRAME_SAMPLES      (OPUS_ENCODE_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000)  // 960

// Max encoded Opus packet that we store/send (actual encoded data is small)
#define OPUS_MAX_PACKET_SIZE 512
// Buffer size required by esp_opus_enc_process (must be >= encoder's expected_out_size)
#define OPUS_ENC_OUTBUF_SIZE 4000

struct OpusPacket {
    uint8_t data[OPUS_MAX_PACKET_SIZE];
    size_t  len;
};

class AudioService {
public:
    using SendCallback = std::function<void(const uint8_t* data, size_t len)>;

    AudioService(AudioCodec* codec);
    ~AudioService();

    void SetSendCallback(SendCallback cb) { on_send_ = cb; }

    bool Start(int decode_sample_rate = 24000);
    void Stop();

    // Push received Opus packet for decoding + playback
    void PushOpusForDecode(const uint8_t* data, size_t len);

    // Control recording
    void StartRecording();
    void StopRecording();
    bool IsRecording() const { return recording_; }

private:
    static void InputTask(void* arg);
    static void OutputTask(void* arg);
    static void CodecTask(void* arg);

    AudioCodec* codec_;
    SendCallback on_send_;

    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    int decode_sample_rate_ = 24000;
    int decode_frame_samples_ = 0;

    QueueHandle_t encode_queue_ = nullptr;  // PCM blocks to encode
    QueueHandle_t decode_queue_ = nullptr;  // Opus packets to decode
    QueueHandle_t playback_queue_ = nullptr; // PCM blocks to play
    QueueHandle_t send_queue_ = nullptr;     // Opus packets to send

    TaskHandle_t input_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;
    TaskHandle_t codec_task_ = nullptr;

    volatile bool running_ = false;
    volatile bool recording_ = false;
};
