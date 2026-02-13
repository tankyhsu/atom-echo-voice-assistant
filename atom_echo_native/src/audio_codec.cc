#include "audio_codec.h"
#include <esp_log.h>

#define TAG "AudioCodec"

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) return;
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Input %s", enable ? "enabled" : "disabled");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) return;
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Output %s", enable ? "enabled" : "disabled");
}

int AudioCodec::ReadSamples(int16_t* dest, int samples) {
    return Read(dest, samples);
}

void AudioCodec::WriteSamples(const int16_t* data, int samples) {
    Write(data, samples);
}
