#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include <cstdlib>

// Opus encoder/decoder - use direct API (not common API) to avoid linking all codecs
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

#define TAG "AudioService"

// Queue depths
#define ENCODE_QUEUE_DEPTH   4
#define DECODE_QUEUE_DEPTH   30
#define PLAYBACK_QUEUE_DEPTH 20
#define SEND_QUEUE_DEPTH     10

// PCM buffer for one Opus frame (960 samples @ 16kHz = 1920 bytes)
struct PcmBlock {
    int16_t samples[OPUS_FRAME_SAMPLES];
    int count;
};

// Larger PCM block for decoded output (may be at higher sample rate)
struct DecodedPcmBlock {
    int16_t samples[2880];  // max: 24kHz * 60ms = 1440 samples
    int count;
};

AudioService::AudioService(AudioCodec* codec) : codec_(codec) {}

AudioService::~AudioService() {
    Stop();
}

bool AudioService::Start(int decode_sample_rate) {
    decode_sample_rate_ = decode_sample_rate;
    decode_frame_samples_ = decode_sample_rate_ * OPUS_FRAME_DURATION_MS / 1000;

    // Create Opus encoder using direct API (16kHz, mono, 60ms frames)
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = OPUS_ENCODE_SAMPLE_RATE;
    enc_cfg.channel = 1;
    enc_cfg.bitrate = 24000;  // 24kbps, good for 16kHz mono voice
    enc_cfg.complexity = 0;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;

    ESP_LOGI(TAG, "Opus enc cfg: sr=%d ch=%d bps=%d br=%d dur=%d cx=%d cfg_sz=%d",
             enc_cfg.sample_rate, enc_cfg.channel, enc_cfg.bits_per_sample,
             enc_cfg.bitrate, enc_cfg.frame_duration, enc_cfg.complexity, (int)sizeof(enc_cfg));
    esp_audio_err_t ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_encoder_);
    if (ret != ESP_AUDIO_ERR_OK || !opus_encoder_) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", ret);
        return false;
    }
    int enc_in_size = 0, enc_out_size = 0;
    esp_opus_enc_get_frame_size(opus_encoder_, &enc_in_size, &enc_out_size);
    ESP_LOGI(TAG, "Opus encoder: %dHz mono, %dms, expected_in=%d expected_out=%d (our_pcm=%d)",
             OPUS_ENCODE_SAMPLE_RATE, OPUS_FRAME_DURATION_MS, enc_in_size, enc_out_size,
             OPUS_FRAME_SAMPLES * (int)sizeof(int16_t));

    // Create Opus decoder using direct API
    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate = (uint32_t)decode_sample_rate_,
        .channel = 1,
        .self_delimited = false,
    };

    ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_decoder_);
    if (ret != ESP_AUDIO_ERR_OK || !opus_decoder_) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %d", ret);
        return false;
    }
    ESP_LOGI(TAG, "Opus decoder: %dHz mono, %dms frames", decode_sample_rate_, OPUS_FRAME_DURATION_MS);

    // Create queues
    encode_queue_ = xQueueCreate(ENCODE_QUEUE_DEPTH, sizeof(PcmBlock*));
    decode_queue_ = xQueueCreate(DECODE_QUEUE_DEPTH, sizeof(OpusPacket*));
    playback_queue_ = xQueueCreate(PLAYBACK_QUEUE_DEPTH, sizeof(DecodedPcmBlock*));
    send_queue_ = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(OpusPacket*));

    running_ = true;

    // Create tasks
    xTaskCreatePinnedToCore(InputTask, "audio_in", 6144, this, 8, &input_task_, 0);
    xTaskCreate(OutputTask, "audio_out", 6144, this, 4, &output_task_);
    xTaskCreate(CodecTask, "opus_codec", 24576, this, 2, &codec_task_);

    ESP_LOGI(TAG, "Audio service started, free heap: %lu", esp_get_free_heap_size());
    return true;
}

void AudioService::Stop() {
    running_ = false;
    recording_ = false;

    if (input_task_) { vTaskDelay(pdMS_TO_TICKS(100)); vTaskDelete(input_task_); input_task_ = nullptr; }
    if (output_task_) { vTaskDelete(output_task_); output_task_ = nullptr; }
    if (codec_task_) { vTaskDelete(codec_task_); codec_task_ = nullptr; }

    // Drain and delete queues
    if (encode_queue_) { PcmBlock* b; while (xQueueReceive(encode_queue_, &b, 0)) free(b); vQueueDelete(encode_queue_); encode_queue_ = nullptr; }
    if (decode_queue_) { OpusPacket* p; while (xQueueReceive(decode_queue_, &p, 0)) free(p); vQueueDelete(decode_queue_); decode_queue_ = nullptr; }
    if (playback_queue_) { DecodedPcmBlock* b; while (xQueueReceive(playback_queue_, &b, 0)) free(b); vQueueDelete(playback_queue_); playback_queue_ = nullptr; }
    if (send_queue_) { OpusPacket* p; while (xQueueReceive(send_queue_, &p, 0)) free(p); vQueueDelete(send_queue_); send_queue_ = nullptr; }

    if (opus_encoder_) { esp_opus_enc_close(opus_encoder_); opus_encoder_ = nullptr; }
    if (opus_decoder_) { esp_opus_dec_close(opus_decoder_); opus_decoder_ = nullptr; }
}

// --- Pipeline stats counters (reset on each playback session) ---
static volatile int stat_rx_frames = 0;      // Opus frames received from server
static volatile int stat_rx_dropped = 0;     // Opus frames dropped (decode queue full)
static volatile int stat_decoded = 0;        // Successfully decoded frames
static volatile int stat_decode_err = 0;     // Decode errors
static volatile int stat_pb_queued = 0;      // Frames queued for playback
static volatile int stat_pb_dropped = 0;     // Frames dropped (playback queue full)
static volatile int stat_played = 0;         // Frames actually played

static void stats_reset() {
    stat_rx_frames = 0; stat_rx_dropped = 0;
    stat_decoded = 0; stat_decode_err = 0;
    stat_pb_queued = 0; stat_pb_dropped = 0;
    stat_played = 0;
}

static void stats_print() {
    ESP_LOGW(TAG, "STATS: rx=%d rx_drop=%d dec=%d dec_err=%d pb_q=%d pb_drop=%d played=%d",
             stat_rx_frames, stat_rx_dropped, stat_decoded, stat_decode_err,
             stat_pb_queued, stat_pb_dropped, stat_played);
}

void AudioService::PushOpusForDecode(const uint8_t* data, size_t len) {
    if (!decode_queue_ || len == 0 || len > OPUS_MAX_PACKET_SIZE) return;

    auto* pkt = (OpusPacket*)malloc(sizeof(OpusPacket));
    if (!pkt) return;
    memcpy(pkt->data, data, len);
    pkt->len = len;

    if (stat_rx_frames == 0) {
        stats_reset();  // Reset all counters on first frame of new session
    }
    stat_rx_frames++;
    if (xQueueSend(decode_queue_, &pkt, 0) != pdTRUE) {
        stat_rx_dropped++;
        free(pkt);
    }
}

void AudioService::StartRecording() {
    codec_->EnableInput(true);
    vTaskDelay(pdMS_TO_TICKS(20));  // Let codec device finish opening
    recording_ = true;
    ESP_LOGI(TAG, "Recording started (codec input_sr=%d, encode_sr=%d)", codec_->input_sample_rate(), OPUS_ENCODE_SAMPLE_RATE);
}

void AudioService::StopRecording() {
    recording_ = false;
    codec_->EnableInput(false);
    ESP_LOGI(TAG, "Recording stopped");
}

// ========== Input Task: Mic → PCM blocks ==========
void AudioService::InputTask(void* arg) {
    auto* self = (AudioService*)arg;
    // Accumulate 960 samples (60ms @ 16kHz)
    // If codec runs at 24kHz, read 1440 samples and downsample
    const int codec_sr = self->codec_->input_sample_rate();
    const int codec_frame = codec_sr * OPUS_FRAME_DURATION_MS / 1000;
    const int read_chunk = codec_sr / 100;  // 10ms chunks

    auto* read_buf = (int16_t*)malloc(codec_frame * sizeof(int16_t));
    int accumulated = 0;

    ESP_LOGI(TAG, "InputTask started: codec_sr=%d, codec_frame=%d, read_chunk=%d", codec_sr, codec_frame, read_chunk);

    while (self->running_) {
        if (!self->recording_) {
            accumulated = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        static bool first_read = true;
        if (first_read) {
            ESP_LOGI(TAG, "InputTask: first ReadSamples call, chunk=%d, dev=%p, input_en=%d",
                     read_chunk, (void*)self->codec_, self->codec_->input_enabled());
            first_read = false;
        }

        // Read 10ms from codec
        self->codec_->ReadSamples(read_buf + accumulated, read_chunk);
        accumulated += read_chunk;

        static bool first_accumulated = true;
        if (first_accumulated) {
            ESP_LOGI(TAG, "InputTask: ReadSamples returned, accumulated=%d/%d", accumulated, codec_frame);
            first_accumulated = false;
        }

        if (accumulated >= codec_frame) {
            static int input_frame_count = 0;
            if (++input_frame_count <= 3) {
                ESP_LOGI(TAG, "InputTask: got %d samples, creating PcmBlock #%d", accumulated, input_frame_count);
            }
            auto* block = (PcmBlock*)malloc(sizeof(PcmBlock));
            if (block) {
                if (codec_sr == OPUS_ENCODE_SAMPLE_RATE) {
                    // Same rate, just copy
                    memcpy(block->samples, read_buf, OPUS_FRAME_SAMPLES * sizeof(int16_t));
                    block->count = OPUS_FRAME_SAMPLES;
                } else {
                    // Simple linear downsampling (e.g., 24kHz → 16kHz)
                    float ratio = (float)codec_sr / OPUS_ENCODE_SAMPLE_RATE;
                    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
                        float src_idx = i * ratio;
                        int idx = (int)src_idx;
                        if (idx >= codec_frame - 1) idx = codec_frame - 2;
                        float frac = src_idx - idx;
                        block->samples[i] = (int16_t)(read_buf[idx] * (1.0f - frac) + read_buf[idx + 1] * frac);
                    }
                    block->count = OPUS_FRAME_SAMPLES;
                }

                if (xQueueSend(self->encode_queue_, &block, 0) != pdTRUE) {
                    free(block);
                }
            }
            accumulated = 0;
        }
    }

    free(read_buf);
    vTaskDelete(NULL);
}

// ========== Output Task: PCM blocks → Speaker ==========
// Codec output stays always-on. Muting is done via hardware amp shutdown pin
// (~10ms) instead of esp_codec_dev_open/close (~50-100ms).
void AudioService::OutputTask(void* arg) {
    auto* self = (AudioService*)arg;
    bool unmuted = false;
    int idle_ticks = 0;
    // Hardware amp mute is fast (~10ms), so we only need a short idle window
    const int MAX_IDLE_TICKS = 10;  // 10 * 10ms = 100ms

    while (self->running_) {
        DecodedPcmBlock* block = nullptr;
        if (xQueueReceive(self->playback_queue_, &block, pdMS_TO_TICKS(10))) {
            if (!unmuted) {
                // Unmute amp via hardware GPIO (fast, ~10ms)
                if (self->on_mute_) self->on_mute_(false);
                unmuted = true;
                // Write silence to let amp stabilize before real audio
                int16_t lead_in[240] = {0};
                for (int i = 0; i < 3; i++) {  // 3 * 240 samples @ 24kHz ≈ 30ms
                    self->codec_->WriteSamples(lead_in, 240);
                }
                ESP_LOGI(TAG, "OutputTask: amp unmuted (30ms lead-in)");
            }
            idle_ticks = 0;
            stat_played++;
            self->codec_->WriteSamples(block->samples, block->count);
            free(block);
        } else if (unmuted) {
            // Queue empty — write silence to keep I2S DMA fed
            int16_t silence[240] = {0};
            self->codec_->WriteSamples(silence, 240);
            idle_ticks++;
            if (idle_ticks >= MAX_IDLE_TICKS) {
                // Drain any remaining frames before muting
                DecodedPcmBlock* drain = nullptr;
                while (xQueueReceive(self->playback_queue_, &drain, 0) == pdTRUE) {
                    stat_played++;
                    self->codec_->WriteSamples(drain->samples, drain->count);
                    free(drain);
                }
                // Mute amp via hardware GPIO (fast, ~10ms)
                if (self->on_mute_) self->on_mute_(true);
                unmuted = false;
                idle_ticks = 0;
                stats_print();
                stats_reset();
                ESP_LOGI(TAG, "OutputTask: amp muted after 100ms idle");
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (unmuted && self->on_mute_) {
        self->on_mute_(true);
    }
    vTaskDelete(NULL);
}

// ========== Codec Task: Opus encode + decode ==========
void AudioService::CodecTask(void* arg) {
    auto* self = (AudioService*)arg;

    // Heap-allocate large buffers to avoid stack overflow
    auto* enc_out_buf = (uint8_t*)malloc(OPUS_ENC_OUTBUF_SIZE);
    auto* dec_out_buf = (int16_t*)malloc(2880 * sizeof(int16_t));
    if (!enc_out_buf || !dec_out_buf) {
        ESP_LOGE(TAG, "CodecTask: failed to allocate buffers");
        free(enc_out_buf);
        free(dec_out_buf);
        vTaskDelete(NULL);
        return;
    }

    while (self->running_) {
        bool did_work = false;

        // --- Decode: Opus → PCM for playback ---
        OpusPacket* opus_pkt = nullptr;
        if (uxQueueMessagesWaiting(self->playback_queue_) < PLAYBACK_QUEUE_DEPTH &&
            xQueueReceive(self->decode_queue_, &opus_pkt, 0) == pdTRUE) {

            esp_audio_dec_in_raw_t raw = {
                .buffer = opus_pkt->data,
                .len = (uint32_t)opus_pkt->len,
                .consumed = 0,
            };
            esp_audio_dec_out_frame_t out = {
                .buffer = (uint8_t*)dec_out_buf,
                .len = (uint32_t)(self->decode_frame_samples_ * sizeof(int16_t)),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t dec_info = {};
            // Use direct Opus decoder API
            esp_audio_err_t ret = esp_opus_dec_decode(
                self->opus_decoder_, &raw, &out, &dec_info);
            free(opus_pkt);

            if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                stat_decoded++;
                int samples = out.decoded_size / sizeof(int16_t);
                auto* pcm = (DecodedPcmBlock*)malloc(sizeof(DecodedPcmBlock));
                if (pcm) {
                    memcpy(pcm->samples, dec_out_buf, out.decoded_size);
                    pcm->count = samples;
                    if (xQueueSend(self->playback_queue_, &pcm, pdMS_TO_TICKS(100)) != pdTRUE) {
                        stat_pb_dropped++;
                        free(pcm);
                    } else {
                        stat_pb_queued++;
                    }
                }
            } else {
                stat_decode_err++;
            }
            did_work = true;
        }

        // --- Encode: PCM → Opus for sending ---
        PcmBlock* pcm_block = nullptr;
        if (uxQueueMessagesWaiting(self->send_queue_) < SEND_QUEUE_DEPTH &&
            xQueueReceive(self->encode_queue_, &pcm_block, 0) == pdTRUE) {

            static int enc_count = 0;
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t*)pcm_block->samples,
                .len = (uint32_t)(pcm_block->count * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = enc_out_buf,
                .len = OPUS_ENC_OUTBUF_SIZE,
                .encoded_bytes = 0,
                .pts = 0,
            };
            // Use direct Opus encoder API
            esp_audio_err_t ret = esp_opus_enc_process(
                self->opus_encoder_, &in, &out);
            free(pcm_block);

            if (ret == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                if (++enc_count <= 5) {
                    ESP_LOGI(TAG, "Encoded frame #%d: %lu bytes, callback=%s",
                             enc_count, out.encoded_bytes, self->on_send_ ? "yes" : "no");
                }
                // Send directly via callback (avoid extra queue)
                if (self->on_send_) {
                    self->on_send_(enc_out_buf, out.encoded_bytes);
                }
            } else if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Opus encode failed: %d", ret);
            }
            did_work = true;
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    free(enc_out_buf);
    free(dec_out_buf);
    vTaskDelete(NULL);
}
