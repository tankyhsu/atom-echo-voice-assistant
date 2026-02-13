#include "es8311_audio_codec.h"
#include <esp_log.h>
#include <cassert>
#include <cstring>

#define TAG "Es8311AudioCodec"

Es8311AudioCodec::Es8311AudioCodec(i2c_master_bus_handle_t i2c_bus, i2c_port_t i2c_port,
                                   int input_sample_rate, int output_sample_rate,
                                   gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                   gpio_num_t dout, gpio_num_t din,
                                   uint8_t es8311_addr, bool use_mclk) {
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // I2S data interface
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // I2C control interface
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_bus,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    // GPIO interface
    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    // ES8311 codec
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = GPIO_NUM_NC;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
    } else {
        ESP_LOGI(TAG, "ES8311 codec initialized");
    }
}

Es8311AudioCodec::~Es8311AudioCodec() {
    if (dev_) esp_codec_dev_delete(dev_);
    if (codec_if_) audio_codec_delete_codec_if(codec_if_);
    if (ctrl_if_) audio_codec_delete_ctrl_if(ctrl_if_);
    if (gpio_if_) audio_codec_delete_gpio_if(gpio_if_);
    if (data_if_) audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk,
                                            gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "I2S duplex channels created (%d Hz)", output_sample_rate_);
}

void Es8311AudioCodec::UpdateDeviceState() {
    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Codec interface is null, cannot update device state");
        return;
    }
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        // set_in_gain may not be supported on all codec versions; ignore error
        esp_codec_dev_set_in_gain(dev_, 30.0);
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        esp_codec_dev_close(dev_);
        dev_ = nullptr;
    }
}

void Es8311AudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    AudioCodec::SetOutputVolume(volume);
    if (dev_) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, volume));
    }
}

void Es8311AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == input_enabled_) return;
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == output_enabled_) return;
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_ && dev_) {
        esp_err_t ret = esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t));
        if (ret != ESP_OK) {
            // Silence the buffer on error (device closing, etc.)
            memset(dest, 0, samples * sizeof(int16_t));
        }
    } else {
        memset(dest, 0, samples * sizeof(int16_t));
    }
    return samples;
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_ && dev_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
