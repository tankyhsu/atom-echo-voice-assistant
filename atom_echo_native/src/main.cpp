#include <M5Unified.h>
#include <M5EchoBase.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <FastLED.h>
#include <math.h>
#include "driver/i2s.h"

const char* ssid     = "oasis";
const char* password = "0a5is402";
const char* mac_ip   = "192.168.31.193";
const int tx_port = 5000;
const int rx_port = 5001;

WiFiUDP udp;
M5EchoBase echobase(I2S_NUM_0);

#define LED_PIN  27
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    leds[0] = CRGB(r, g, b);
    FastLED.show();
}

void breatheLED(uint8_t r, uint8_t g, uint8_t b) {
    float phase = (float)(millis() % 2000) / 2000.0f;
    float bright = (sinf(phase * 2.0f * M_PI) + 1.0f) / 2.0f;
    bright = 0.05f + bright * 0.95f;
    leds[0] = CRGB((uint8_t)(r * bright), (uint8_t)(g * bright), (uint8_t)(b * bright));
    FastLED.show();
}

// --- Recording ---
#define REC_I2S_BYTES 2048
static uint8_t  rec_stereo[REC_I2S_BYTES];
static int16_t  rec_mono[REC_I2S_BYTES / 8];

// ========== RING BUFFER (lock-free SPSC) ==========
#define RING_SIZE (60 * 1024)
static uint8_t* ring_buf = NULL;
static volatile size_t ring_head = 0;
static volatile size_t ring_tail = 0;

size_t ring_avail() {
    size_t h = ring_head, t = ring_tail;
    return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}

size_t ring_free() {
    return RING_SIZE - 1 - ring_avail();
}

size_t ring_write(const uint8_t* data, size_t len) {
    size_t f = ring_free();
    if (len > f) len = f;
    if (!len) return 0;
    size_t h = ring_head;
    size_t space = RING_SIZE - h;
    if (space >= len) {
        memcpy(ring_buf + h, data, len);
    } else {
        memcpy(ring_buf + h, data, space);
        memcpy(ring_buf, data + space, len - space);
    }
    ring_head = (h + len) % RING_SIZE;
    return len;
}

size_t ring_read(uint8_t* dst, size_t len) {
    size_t a = ring_avail();
    if (len > a) len = a;
    if (!len) return 0;
    size_t t = ring_tail;
    size_t space = RING_SIZE - t;
    if (space >= len) {
        memcpy(dst, ring_buf + t, len);
    } else {
        memcpy(dst, ring_buf + t, space);
        memcpy(dst + space, ring_buf, len - space);
    }
    ring_tail = (t + len) % RING_SIZE;
    return len;
}

// ========== PLAYBACK TASK (Core 0) ==========
static volatile bool play_active = false;
static volatile bool need_mute   = false;

static void playbackTask(void* param) {
    static uint8_t  raw[1024];
    static int32_t  stereo[512 * 2];
    unsigned long last_data = 0;
    bool first_chunk = true;

    while (true) {
        if (!play_active) {
            first_chunk = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t avail = ring_avail();

        // Wait for 2KB buffer before starting (absorb WiFi jitter)
        if (first_chunk && avail < 2048) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (first_chunk) {
            // Write silence to I2S so PA stabilizes before real audio
            int32_t zeros[64] = {0};
            for (int i = 0; i < 25; i++) {  // ~50ms
                size_t w;
                i2s_write(I2S_NUM_0, zeros, sizeof(zeros), &w, portMAX_DELAY);
            }
            first_chunk = false;
        }

        if (avail >= 2) {
            size_t to_read = (avail > 1024) ? 1024 : (avail & ~1);
            size_t got = ring_read(raw, to_read);
            last_data = millis();

            const int16_t* mono = (const int16_t*)raw;
            int samples = got / 2;
            for (int i = 0; i < samples; i++) {
                int32_t val = (int32_t)mono[i] << 16;
                stereo[i * 2]     = val;
                stereo[i * 2 + 1] = val;
            }
            size_t written;
            i2s_write(I2S_NUM_0, (uint8_t*)stereo, samples * 8, &written, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
            if (last_data > 0 && millis() - last_data > 500) {
                vTaskDelay(pdMS_TO_TICKS(200));
                need_mute   = true;
                play_active = false;
                last_data   = 0;
                Serial.println("Playback done.");
            }
        }
    }
}

// ========== CHIME ==========
void writeSilence(int ms) {
    int32_t zeros[64] = {0};
    int chunks = ms / 2;
    for (int i = 0; i < chunks; i++) {
        size_t w;
        i2s_write(I2S_NUM_0, zeros, sizeof(zeros), &w, portMAX_DELAY);
    }
}

void playChime() {
    // LED debug: RED=note1, GREEN=note2, BLUE=note3
    const float freqs[]  = {700.0f, 1000.0f, 1400.0f};  // All speaker-friendly
    const int   dur_ms[] = {200, 200, 250};
    const int   gap_ms   = 120;
    const int   sr = 16000;
    const int   CHUNK = 128;
    int32_t chunk[CHUNK * 2];
    const uint8_t colors[][3] = {{60,0,0}, {0,60,0}, {0,0,60}};  // R, G, B

    echobase.setMute(false);
    delay(50);
    writeSilence(200);

    for (int n = 0; n < 3; n++) {
        setLED(colors[n][0], colors[n][1], colors[n][2]);  // Light up for this note
        int samples = sr * dur_ms[n] / 1000;
        int fade = samples / 6;
        int offset = 0;
        Serial.printf("chime %d: %.0fHz %dms\n", n, freqs[n], dur_ms[n]);

        while (offset < samples) {
            int m = samples - offset;
            if (m > CHUNK) m = CHUNK;
            for (int i = 0; i < m; i++) {
                int idx = offset + i;
                float env = 1.0f;
                if (idx < fade)            env = (float)idx / fade;
                if (idx > samples - fade)  env = (float)(samples - idx) / fade;
                int16_t s = (int16_t)(sinf(2.0f * M_PI * freqs[n] * ((float)idx / sr)) * 14000.0f * env);
                int32_t val = (int32_t)s << 16;
                chunk[i * 2]     = val;
                chunk[i * 2 + 1] = val;
            }
            size_t w;
            i2s_write(I2S_NUM_0, (uint8_t*)chunk, m * 8, &w, portMAX_DELAY);
            offset += m;
        }
        setLED(0, 0, 0);  // LED off during gap
        if (n < 2) writeSilence(gap_ms);
    }
    writeSilence(300);
    delay(300);
    echobase.setMute(true);
}

// ========== SETUP & LOOP ==========
bool is_recording = false;

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    M5.begin(cfg);
    Serial.begin(115200);

    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(40);

    echobase.init(16000, 25, 21, 23, 19, 22, 33, Wire);
    echobase.setSpeakerVolume(70);
    echobase.setMicGain(ES8311_MIC_GAIN_18DB);

    delay(100);
    Serial.printf("Heap: %d\n", ESP.getFreeHeap());
    playChime();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        breatheLED(0, 60, 0);
        delay(30);
    }
    Serial.println("WiFi OK");
    Serial.printf("Heap: %d, max_alloc: %d\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    ring_buf = (uint8_t*)malloc(RING_SIZE);
    if (!ring_buf) Serial.println("ERR: ring alloc fail");
    else Serial.printf("Ring OK (%dKB)\n", RING_SIZE / 1024);

    xTaskCreatePinnedToCore(playbackTask, "play", 4096, NULL, 5, NULL, 0);

    udp.begin(rx_port);
    setLED(40, 40, 40);
    Serial.println("Ready.");
}

void loop() {
    M5.update();

    if (need_mute) {
        echobase.setMute(true);
        setLED(40, 40, 40);
        need_mute = false;
    }

    // --- Receive UDP → ring buffer ---
    int pktSize = udp.parsePacket();
    if (pktSize > 0 && !is_recording) {
        uint8_t pkt[1024];
        int len = udp.read(pkt, min(pktSize, 1024));
        if (len > 0 && ring_buf) {
            if (!play_active) {
                ring_head = ring_tail = 0;
                echobase.setMute(false);
                delay(20);
                play_active = true;
                setLED(60, 60, 60);
                Serial.printf("Stream start (heap=%d)\n", ESP.getFreeHeap());
            }
            size_t wrote = ring_write((uint8_t*)pkt, len);
            if ((int)wrote < len)
                Serial.printf("WARN: ring full, lost %d\n", len - (int)wrote);
        }
    }

    // --- Recording ---
    if (M5.BtnA.isPressed()) {
        if (!is_recording) {
            is_recording = true;
            play_active = false;
            echobase.setMute(true);
            delay(10);
            setLED(60, 0, 0);
            Serial.println("Rec...");
        }
        echobase.record(rec_stereo, REC_I2S_BYTES);
        int32_t* st32 = (int32_t*)rec_stereo;
        int frames = REC_I2S_BYTES / 8;
        for (int i = 0; i < frames; i++) {
            rec_mono[i] = (int16_t)(st32[i * 2 + 1] >> 16);
        }
        udp.beginPacket(mac_ip, tx_port);
        udp.write((uint8_t*)rec_mono, frames * 2);
        udp.endPacket();
    } else {
        if (is_recording) {
            is_recording = false;
            setLED(60, 30, 0);
            Serial.println("Stop. Waiting...");
        }
    }

    delay(1);
}
