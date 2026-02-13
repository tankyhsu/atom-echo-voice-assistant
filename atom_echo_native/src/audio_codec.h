#pragma once

#include <driver/i2s_std.h>
#include <cstdint>

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

class AudioCodec {
public:
    AudioCodec() = default;
    virtual ~AudioCodec() = default;

    virtual void SetOutputVolume(int volume);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    int ReadSamples(int16_t* dest, int samples);
    void WriteSamples(const int16_t* data, int samples);

    int input_sample_rate() const { return input_sample_rate_; }
    int output_sample_rate() const { return output_sample_rate_; }
    int output_volume() const { return output_volume_; }
    bool input_enabled() const { return input_enabled_; }
    bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int output_volume_ = 70;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};
