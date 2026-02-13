#include <cstdio>
#include <cstring>
#include <cmath>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/rmt_tx.h>

#include "es8311_audio_codec.h"
#include "audio_service.h"
#include "ws_transport.h"

#define TAG "main"

// ========== Hardware Pin Definitions (Atom Echo + Echo Base) ==========
// I2S
#define I2S_BCK   GPIO_NUM_33
#define I2S_WS    GPIO_NUM_19
#define I2S_DOUT  GPIO_NUM_22
#define I2S_DIN   GPIO_NUM_23

// I2C (Echo Base ES8311 + PI4IOE)
#define I2C_SDA   GPIO_NUM_25
#define I2C_SCL   GPIO_NUM_21

// LED (SK6812 on Atom Echo)
#define LED_PIN   GPIO_NUM_27

// Button
#define BTN_PIN   GPIO_NUM_39

// ES8311 address (8-bit format: esp_codec_dev right-shifts by 1 internally)
#define ES8311_ADDR 0x30

// PI4IOE I/O expander
#define PI4IOE_ADDR          0x43
#define PI4IOE_REG_IO_PP     0x07
#define PI4IOE_REG_IO_DIR    0x03
#define PI4IOE_REG_IO_OUT    0x05
#define PI4IOE_REG_IO_PULLUP 0x0D

// Audio config
#define SAMPLE_RATE 24000

// ========== WiFi Config ==========
#define WIFI_SSID     "oasis"
#define WIFI_PASSWORD "0a5is402"
#define BACKEND_IP    "192.168.31.193"

// WebSocket URI
#define WS_URI "ws://" BACKEND_IP ":8765"

// ========== Globals ==========
static i2c_master_bus_handle_t i2c_bus = nullptr;
static i2c_master_dev_handle_t pi4ioe_dev = nullptr;
static Es8311AudioCodec* codec = nullptr;
static AudioService* audio_svc = nullptr;
static WsTransport* ws = nullptr;

static volatile bool wifi_connected = false;

// ========== PI4IOE I/O Expander ==========
static void pi4ioe_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    i2c_master_transmit(pi4ioe_dev, buf, 2, 100);
}

static void pi4ioe_init() {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IOE_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &pi4ioe_dev));

    pi4ioe_write_reg(PI4IOE_REG_IO_PP, 0x00);
    pi4ioe_write_reg(PI4IOE_REG_IO_PULLUP, 0xFF);
    pi4ioe_write_reg(PI4IOE_REG_IO_DIR, 0x6F);
    pi4ioe_write_reg(PI4IOE_REG_IO_OUT, 0xFF);
    ESP_LOGI(TAG, "PI4IOE initialized");
}

static void set_speaker_mute(bool mute) {
    pi4ioe_write_reg(PI4IOE_REG_IO_OUT, mute ? 0x00 : 0xFF);
}

// ========== SK6812 LED via RMT ==========
static rmt_channel_handle_t led_channel = nullptr;
static rmt_encoder_handle_t led_encoder = nullptr;

static void led_init() {
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,  // 10MHz = 100ns per tick
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &led_channel));

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .duration0 = 3,   // 300ns high
            .level0 = 1,
            .duration1 = 9,   // 900ns low
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9,   // 900ns high
            .level0 = 1,
            .duration1 = 3,   // 300ns low
            .level1 = 0,
        },
        .flags = {
            .msb_first = 1,
        },
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    // SK6812 order: GRB
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    rmt_transmit(led_channel, led_encoder, grb, sizeof(grb), &tx_cfg);
    rmt_tx_wait_all_done(led_channel, 100);
}

// ========== I2C Init ==========
static void i2c_init() {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    ESP_LOGI(TAG, "I2C bus initialized on SDA=%d SCL=%d", I2C_SDA, I2C_SCL);

    // Small delay for bus to settle
    vTaskDelay(pdMS_TO_TICKS(100));

    // Probe specific expected devices
    esp_err_t ret;
    ret = i2c_master_probe(i2c_bus, 0x18, pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "  Probe ES8311 (0x18): %s", ret == ESP_OK ? "FOUND" : esp_err_to_name(ret));
    ret = i2c_master_probe(i2c_bus, 0x43, pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "  Probe PI4IOE (0x43): %s", ret == ESP_OK ? "FOUND" : esp_err_to_name(ret));
}

// ========== WiFi ==========
static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi connecting to %s...", WIFI_SSID);
}

// ========== Chime (direct codec write) ==========
static void play_chime() {
    codec->EnableOutput(true);
    set_speaker_mute(false);
    vTaskDelay(pdMS_TO_TICKS(50));

    const float freqs[] = {700.0f, 1000.0f, 1400.0f};
    const int dur_ms[] = {200, 200, 250};
    const int gap_ms = 120;
    const int sr = SAMPLE_RATE;
    const uint8_t colors[][3] = {{60,0,0}, {0,60,0}, {0,0,60}};

    // Write initial silence to stabilize PA
    int16_t silence[240] = {0};
    for (int i = 0; i < 10; i++) {
        codec->WriteSamples(silence, 240);
    }

    for (int n = 0; n < 3; n++) {
        led_set(colors[n][0], colors[n][1], colors[n][2]);

        int total_samples = sr * dur_ms[n] / 1000;
        int fade = total_samples / 6;
        int16_t buf[240];
        int offset = 0;

        while (offset < total_samples) {
            int count = total_samples - offset;
            if (count > 240) count = 240;
            for (int i = 0; i < count; i++) {
                int idx = offset + i;
                float env = 1.0f;
                if (idx < fade) env = (float)idx / fade;
                if (idx > total_samples - fade) env = (float)(total_samples - idx) / fade;
                buf[i] = (int16_t)(sinf(2.0f * M_PI * freqs[n] * ((float)idx / sr)) * 6000.0f * env);
            }
            codec->WriteSamples(buf, count);
            offset += count;
        }

        led_set(0, 0, 0);
        if (n < 2) {
            int gap_samples = sr * gap_ms / 1000;
            int16_t gap_buf[240] = {0};
            for (int s = 0; s < gap_samples; s += 240) {
                int c = gap_samples - s;
                if (c > 240) c = 240;
                codec->WriteSamples(gap_buf, c);
            }
        }
    }

    // Trailing silence
    for (int i = 0; i < 15; i++) {
        codec->WriteSamples(silence, 240);
    }

    set_speaker_mute(true);
    codec->EnableOutput(false);
    ESP_LOGI(TAG, "Chime done");
}

// ========== Main ==========
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Atom Echo Voice Assistant starting...");

    // NVS init (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // LED
    led_init();
    led_set(20, 20, 0);  // Yellow = starting

    // I2C
    i2c_init();

    // PI4IOE I/O expander (controls speaker mute)
    pi4ioe_init();
    set_speaker_mute(true);

    // Audio codec (ES8311 via esp_codec_dev)
    codec = new Es8311AudioCodec(
        i2c_bus, I2C_NUM_1,
        SAMPLE_RATE, SAMPLE_RATE,
        GPIO_NUM_NC,  // MCLK not connected, derived from BCK
        I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN,
        ES8311_ADDR, false  // use_mclk = false
    );
    codec->SetOutputVolume(95);
    ESP_LOGI(TAG, "Codec initialized, free heap: %lu", esp_get_free_heap_size());

    // Play startup chime
    play_chime();

    // WiFi
    wifi_init();
    led_set(0, 40, 0);  // Green = connecting WiFi

    // Wait for WiFi
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    led_set(20, 20, 20);  // White = ready
    ESP_LOGI(TAG, "WiFi ready. Free heap: %lu", esp_get_free_heap_size());

    // Unmute speaker for playback
    set_speaker_mute(false);

    // Audio service (Opus encode/decode pipeline)
    audio_svc = new AudioService(codec);

    // WebSocket transport
    ws = new WsTransport();

    // Wire: received Opus from server → decode → play
    ws->SetAudioCallback([](const uint8_t* data, size_t len) {
        audio_svc->PushOpusForDecode(data, len);
    });

    // Wire: server JSON messages → LED state changes
    ws->SetJsonCallback([](const char* json, size_t len) {
        // Copy to null-terminated buffer for strstr
        char buf[128];
        size_t copy_len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, json, copy_len);
        buf[copy_len] = '\0';

        if (strstr(buf, "\"tts_start\"")) {
            led_set(0, 40, 40);  // Cyan = playing TTS
        } else if (strstr(buf, "\"tts_end\"")) {
            led_set(0, 20, 40);  // Blue-cyan = idle/connected
        } else if (strstr(buf, "\"stt\"")) {
            led_set(40, 40, 0);  // Yellow = got STT, waiting for LLM
        }
    });

    // Wire: encoded Opus from mic → send to server
    audio_svc->SetSendCallback([](const uint8_t* data, size_t len) {
        ws->SendAudio(data, len);
    });

    // Start audio service
    audio_svc->Start(SAMPLE_RATE);
    ESP_LOGI(TAG, "Audio service started. Free heap: %lu", esp_get_free_heap_size());

    // Connect WebSocket
    ws->Connect(WS_URI);

    // Wait for WS connection
    for (int i = 0; i < 100 && !ws->IsConnected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ws->IsConnected()) {
        ESP_LOGI(TAG, "WebSocket connected to %s", WS_URI);
        // Send hello
        const char* hello = "{\"type\":\"hello\",\"audio\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
        ws->SendJson(hello, strlen(hello));
    } else {
        ESP_LOGW(TAG, "WebSocket connection timeout");
    }
    led_set(0, 20, 40);  // Cyan = connected

    ESP_LOGI(TAG, "Ready. Free heap: %lu", esp_get_free_heap_size());

    // ========== Main Loop: Button handling ==========
    gpio_set_direction(BTN_PIN, GPIO_MODE_INPUT);
    bool btn_pressed = false;
    int loop_count = 0;

    while (true) {
        bool btn = (gpio_get_level(BTN_PIN) == 0);  // Active low

        // Log button state every 2 seconds for first 20 seconds
        if (++loop_count % 100 == 0 && loop_count <= 1000) {
            ESP_LOGI(TAG, "btn_gpio=%d pressed=%d recording=%d", !btn ? 1 : 0, btn_pressed, audio_svc->IsRecording());
        }

        if (btn && !btn_pressed) {
            // Button press → start recording
            btn_pressed = true;
            ESP_LOGI(TAG, "=== BUTTON PRESSED ===");
            audio_svc->StartRecording();
            led_set(60, 0, 0);  // Red = recording
            // Notify server
            const char* start = "{\"type\":\"record_start\"}";
            ws->SendJson(start, strlen(start));
        } else if (!btn && btn_pressed) {
            // Button release → stop recording
            btn_pressed = false;
            ESP_LOGI(TAG, "=== BUTTON RELEASED ===");
            audio_svc->StopRecording();
            led_set(60, 30, 0);  // Orange = processing
            // Notify server
            const char* stop = "{\"type\":\"record_stop\"}";
            ws->SendJson(stop, strlen(stop));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
