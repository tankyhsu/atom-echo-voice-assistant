#pragma once

#include "audio_codec.h"
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>

class Es8311AudioCodec : public AudioCodec {
public:
    Es8311AudioCodec(i2c_master_bus_handle_t i2c_bus, i2c_port_t i2c_port,
                     int input_sample_rate, int output_sample_rate,
                     gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                     gpio_num_t dout, gpio_num_t din,
                     uint8_t es8311_addr, bool use_mclk = false);
    ~Es8311AudioCodec() override;

    void SetOutputVolume(int volume) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

private:
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);
    void UpdateDeviceState();

    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t dev_ = nullptr;
    std::mutex mutex_;
};
