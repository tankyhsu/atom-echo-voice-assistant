# M5Stack Atom Echo Voice Assistant - Development Log

## Project Overview

A voice assistant built on M5Stack Atom Echo with Atomic Echo Base (SKU A149), communicating with a Python backend over UDP. The user presses the button to record, audio is sent to a Mac for processing (STT -> LLM -> TTS), and the response audio streams back to the device for playback.

## Hardware

- **MCU**: ESP32-PICO-D4 (dual-core 240MHz, 520KB SRAM, 4MB Flash)
- **Audio Codec**: ES8311 (24-bit, I2C control)
- **Amplifier**: NS4150B (Class-D, 3W)
- **Microphone**: MEMS (on Echo Base board)
- **LED**: SK6812 (NeoPixel-compatible, GPIO27)
- **Button**: GPIO39 (via M5.BtnA)

### Pin Mapping (Echo Base)

| Function | GPIO |
|----------|------|
| I2S BCK  | 19   |
| I2S WS   | 33   |
| I2S DO   | 22   |
| I2S DI   | 23   |
| I2C SDA  | 25   |
| I2C SCL  | 21   |
| LED      | 27   |

## Architecture

```
[Atom Echo] --UDP:5000--> [Mac: voice_assistant.py] --SiliconFlow API--> [STT/LLM/TTS]
[Atom Echo] <--UDP:5001-- [Mac: voice_assistant.py] <---------------------- [TTS audio]
```

### ESP32 Firmware (`atom_echo_native/src/main.cpp`)

- **Framework**: Arduino + ESP-IDF (PlatformIO)
- **Dual-core architecture**:
  - Core 1 (main loop): UDP receive, button handling, LED, I2C mute control
  - Core 0 (FreeRTOS task): Audio playback from ring buffer to I2S
- **Ring buffer**: 60KB lock-free SPSC (Single Producer Single Consumer) for streaming audio
- **Audio format**: 32-bit stereo I2S (mono 16-bit PCM converted to 32-bit stereo for ES8311)

### Python Backend (`voice_assistant.py`)

- Receives 16-bit mono PCM over UDP (port 5000)
- Pipeline: STT (SenseVoiceSmall) -> LLM (DeepSeek-V3) -> TTS (CosyVoice2)
- Streams TTS audio back over UDP (port 5001) at ~1.14x real-time (28ms per 1024-byte chunk)

## Key Technical Discoveries & Fixes

### 1. ES8311 Codec Clock Mismatch (ROOT CAUSE of audio distortion)

**Problem**: The M5EchoBase library initializes the ES8311 codec with `ES8311_RESOLUTION_32` (32-bit) for both input and output, but configured I2S with `I2S_BITS_PER_SAMPLE_16BIT`.

**Impact**: With 16-bit I2S at 16kHz stereo, BCK = 16000 x 16 x 2 = 512,000 Hz. But the ES8311 uses BCK as MCLK source (`mclk_from_mclk_pin = false`), and its coefficient table has an entry for {1024000, 16000} but NOT {512000, 16000}. This caused the codec's entire internal clock tree to be misconfigured, producing harsh/distorted audio.

**Fix**: Modified library file `M5EchoBase.cpp` line 110:
```cpp
// BEFORE (original):
i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
// AFTER (fixed):
i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
```

Now BCK = 16000 x 32 x 2 = 1,024,000 Hz, matching the ES8311's expected MCLK.

**File**: `atom_echo_native/.pio/libdeps/m5stack-atom/M5Atomic-EchoBase/src/M5EchoBase.cpp`

### 2. Library `play()` Method Kills Audio

**Problem**: `echobase.play()` calls `i2s_zero_dma_buffer()` immediately after `i2s_write()`, which zeroes out DMA buffers while audio is still playing.

**Fix**: Bypass `echobase.play()` entirely and call `i2s_write()` directly.

### 3. 32-bit I2S Data Format

Since I2S is now 32-bit stereo, audio data must be formatted as:
```
[Left_32bit][Right_32bit] per frame = 8 bytes per frame
```

16-bit mono PCM is converted by left-shifting to MSB:
```cpp
int32_t val = (int32_t)mono_sample << 16;  // 16-bit -> MSB of 32-bit
stereo[i * 2]     = val;  // Left channel
stereo[i * 2 + 1] = val;  // Right channel (same as left)
```

### 4. Recording Format

The ES8311 outputs 32-bit stereo I2S. The microphone data is in the right channel, top 16 bits:
```cpp
rec_mono[i] = (int16_t)(st32[i * 2 + 1] >> 16);  // Right ch, top 16 bits
```

### 5. Buffer Overflow -> Streaming Architecture

**Problem**: Initial design buffered all audio before playback (80KB buffer = ~2.5s max). Longer TTS responses were truncated, losing most words.

**Fix**: Switched to streaming architecture:
- FreeRTOS playback task on Core 0 consumes from ring buffer
- Main loop on Core 1 receives UDP and writes to ring buffer
- Python sends at ~1.14x real-time (28ms per 1024-byte chunk) instead of 16x
- Ring buffer (60KB) smooths out network jitter
- Any length audio can play without buffer overflow

### 6. Startup Chime First Note Attenuation

**Problem**: The first note of the startup chime is always quieter/shorter than subsequent notes.

**Status**: Partially resolved. The PA (NS4150B) and/or ES8311 codec need warmup time. A 200ms silence is sent before the first note, but the first note still has slight attenuation. Frequencies below 500Hz are very weak on the tiny speaker.

**Current chime**: 700Hz / 1000Hz / 1400Hz ascending, with LED colors (R/G/B) for debugging.

## Current State

### Working Features
- Voice recording (button press) and UDP transmission to Mac
- Full STT -> LLM -> TTS pipeline via SiliconFlow API
- Streaming audio playback (any length)
- Startup chime (3-note ascending, first note slightly weak)
- LED indicators:
  - Green breathing: WiFi connecting
  - Red: Recording
  - Orange: Processing/waiting
  - White: Playing audio
  - Dim white: Idle

### Known Issues
1. **Chime first note**: Slightly attenuated compared to notes 2 and 3. Likely PA/codec warmup issue or small speaker low-frequency rolloff.
2. **Voice first word**: Occasionally the first word of TTS playback is slightly unclear. Mitigated by 50ms silence warmup and 2KB initial buffer in playback task.
3. **Occasional audio breaks**: Minor stutters during long playback, likely WiFi jitter exceeding ring buffer headroom.

### File Inventory

| File | Description |
|------|-------------|
| `atom_echo_native/src/main.cpp` | ESP32 firmware (streaming playback, ring buffer, FreeRTOS) |
| `atom_echo_native/platformio.ini` | PlatformIO build config |
| `voice_assistant.py` | Python backend (STT/LLM/TTS pipeline) |
| `secrets.yaml` | API keys (gitignored) |
| `atom_echo.yaml` | Legacy ESPHome config (not used in current build) |

### Modified Library File

`atom_echo_native/.pio/libdeps/m5stack-atom/M5Atomic-EchoBase/src/M5EchoBase.cpp`:
- Line 110: `I2S_BITS_PER_SAMPLE_16BIT` -> `I2S_BITS_PER_SAMPLE_32BIT`
- **WARNING**: This file lives in `.pio/libdeps/` which is gitignored and regenerated on `pio lib install`. The fix must be reapplied after clean builds. Consider forking the library or using a build script to patch it.

### Configuration

- WiFi SSID: `oasis`
- Mac IP: `192.168.31.193`
- UDP ports: 5000 (ESP32->Mac), 5001 (Mac->ESP32)
- Sample rate: 16000 Hz
- Speaker volume: 70/100
- Mic gain: 18dB
- Python audio boost: +6dB, 30ms fade in/out

## Build & Deploy

```bash
# Build and upload firmware
cd atom_echo_native
/Users/txu35/Library/Python/3.9/bin/pio run -t upload --upload-port /dev/cu.usbserial-95529C653B

# Run Python backend
python3 voice_assistant.py

# Serial monitor (use external terminal, not PlatformIO in non-interactive shell)
screen /dev/cu.usbserial-95529C653B 115200
```

## Future Plans
- Replace LLM backend with OpenClaw
- Multi-WiFi support (connect to multiple networks)
- Improve chime first-note issue
- Consider forking M5Atomic-EchoBase library to permanently fix the 32-bit I2S issue
