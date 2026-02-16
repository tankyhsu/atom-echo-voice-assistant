# Atom Echo Voice Assistant

A hardware voice assistant built on M5Stack Atom Echo with cloud AI backend. Press and hold the button to speak, release to automatically perform speech recognition, LLM inference, text-to-speech synthesis, and play the response.

Developed entirely with ESP-IDF native framework (not Arduino), using Opus codec + WebSocket for low-latency bidirectional audio streaming.

## Features

- **Push-to-Talk**: Hold button to record, release to process
- **Full Voice Pipeline**: STT → LLM → TTS, end-to-end voice interaction
- **Opus Audio Streaming**: Low-bandwidth, high-quality real-time audio transport
- **NanoBot Integration**: Supports LLM Agent streaming events (thinking / tool_call / tool_result)
- **LED Status Indicator**: Real-time device status via SK6812 RGB LED
- **Notification Sounds**: Unique audio cues for LLM thinking, tool calls, and tool results
- **Multi-WiFi Support**: Automatic failover across multiple WiFi networks
- **Zero Frame Loss**: Carefully tuned audio pipeline, verified rx == dec == pb_q == played
- **Docker Deployment**: Server supports one-command Docker deployment

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Server (Raspberry Pi / PC)                │
│                                                              │
│  ┌─────────────────────┐    ┌─────────────────────────────┐ │
│  │  voice_assistant.py  │    │  NanoBot (LLM Agent)        │ │
│  │  (aiohttp WS :8765) │◄──►│  (WS :18790/ws/chat)        │ │
│  │                      │    │  Claude + MCP tools          │ │
│  │  STT ──► LLM ──► TTS│    └─────────────────────────────┘ │
│  └──────────┬───────────┘                                    │
│             │ WebSocket (binary: Opus, text: JSON)           │
└─────────────┼────────────────────────────────────────────────┘
              │ WiFi
┌─────────────┴────────────────────────────────────────────────┐
│                 M5Stack Atom Echo + Echo Base                  │
│                                                                │
│  Button ──► Mic ──► Opus Encode ──► WS Send                   │
│  WS Recv ──► Opus Decode ──► PCM Playback ──► Speaker         │
│                                                                │
│  ESP32 (ESP-IDF via PlatformIO)                                │
│  3 FreeRTOS Tasks: InputTask / CodecTask / OutputTask          │
└────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
Recording: Button Press → Mic (24kHz) → Downsample 16kHz → Opus Encode → WebSocket
Playback:  WebSocket → Opus Decode → PCM 24kHz → I2S DMA → ES8311 DAC → Speaker
```

### Communication Protocol (ESP32 ↔ Server)

| Direction | Type | Format | Description |
|-----------|------|--------|-------------|
| ESP→Server | Binary | Opus packet | Mic audio frame (16kHz, 60ms) |
| Server→ESP | Binary | Opus packet | TTS audio frame (24kHz, 60ms) |
| ESP→Server | Text | `{"type":"hello","audio":{...}}` | Device online |
| ESP→Server | Text | `{"type":"record_start"}` | Button pressed |
| ESP→Server | Text | `{"type":"record_stop"}` | Button released |
| Server→ESP | Text | `{"type":"stt","text":"..."}` | Speech recognition result |
| Server→ESP | Text | `{"type":"status","stage":"..."}` | LLM processing status |
| Server→ESP | Text | `{"type":"tts_start"}` | TTS playback starting |
| Server→ESP | Text | `{"type":"tts_end"}` | TTS playback finished |

## Hardware Requirements

### Devices

- **M5Stack Atom Echo** — ESP32-PICO-D4 dev board with built-in MEMS microphone and button
- **M5Stack Echo Base** — Expansion base with ES8311 audio codec + NS4150B amplifier + speaker

### Components

| Component | Model | Interface | Description |
|-----------|-------|-----------|-------------|
| SoC | ESP32-PICO-D4 | — | Dual-core 240MHz, 520KB SRAM, 4MB Flash |
| Audio Codec | ES8311 | I2C + I2S | 24-bit ADC/DAC |
| I/O Expander | PI4IOE5V6408 | I2C (0x43) | Speaker mute control |
| Amplifier | NS4150B | — | 3W Class D |
| LED | SK6812 | GPIO27 (RMT) | RGB status indicator |
| Button | — | GPIO39 | Active Low |
| Microphone | SPM1423 | I2S | MEMS microphone |

### Server

- Raspberry Pi 4/5 or any Linux/macOS host
- Python 3.9+
- libopus, ffmpeg

## Project Structure

```
.
├── atom_echo_native/              # ESP32 firmware (ESP-IDF)
│   ├── platformio.ini             # PlatformIO config
│   ├── partitions.csv             # Custom partition table (3MB app)
│   ├── sdkconfig.defaults         # ESP-IDF default config
│   └── src/
│       ├── main.cc                # Entry point, hardware init, WiFi, button, LED
│       ├── audio_codec.h/cc       # Audio codec abstract base class
│       ├── es8311_audio_codec.h/cc # ES8311 codec implementation
│       ├── audio_service.h/cc     # Core audio pipeline (3 FreeRTOS tasks)
│       ├── ws_transport.h/cc      # WebSocket transport layer
│       └── idf_component.yml      # Component dependencies
├── voice_assistant.py             # Server backend (Python)
├── requirements.txt               # Python dependencies
├── secrets.yaml.example           # Config template
├── Dockerfile                     # Docker image
├── docker-compose.yml             # Docker Compose deployment
└── ARCHITECTURE.md                # Detailed technical architecture
```

## Quick Start

### 1. Prepare Configuration

Copy the config template and fill in your details:

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml`:

```yaml
siliconflow_api_key: "sk-your-api-key-here"
```

You need a [SiliconFlow](https://cloud.siliconflow.cn/i/vnNqBaFz) API key for STT (SenseVoiceSmall) and TTS (CosyVoice2).

### 2. Build and Flash ESP32 Firmware

#### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) installed
- USB cable connected to Atom Echo

#### Modify Firmware Configuration

Edit `atom_echo_native/src/main.cc` and update:

```cpp
// WiFi configuration
static const WiFiCredential wifi_list[] = {
    {"your_wifi_ssid",  "your_wifi_password"},
};

// Server IP (machine running voice_assistant.py)
#define BACKEND_IP "192.168.x.x"
```

#### Build and Flash

```bash
# Build
pio run -e m5stack-atom -d atom_echo_native

# Flash (replace with your serial port)
pio run -e m5stack-atom -t upload \
  --upload-port /dev/cu.usbserial-XXXXX -d atom_echo_native

# Monitor serial output
pio device monitor --port /dev/cu.usbserial-XXXXX --baud 115200
```

### 3. Deploy Server

#### Option A: Direct Run

```bash
# Install system dependencies
# macOS:
brew install opus ffmpeg

# Debian/Ubuntu (Raspberry Pi):
sudo apt install -y libopus-dev ffmpeg

# Install Python dependencies
pip3 install -r requirements.txt

# Run
python3 voice_assistant.py
```

#### Option B: Docker

```bash
docker compose up -d
```

#### Option C: Systemd Service (Recommended for Raspberry Pi)

```bash
sudo mkdir -p /opt/voice-assistant
sudo cp voice_assistant.py requirements.txt secrets.yaml /opt/voice-assistant/

sudo tee /etc/systemd/system/voice-assistant.service << 'EOF'
[Unit]
Description=Voice Assistant
After=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/voice-assistant
ExecStart=/usr/bin/python3 voice_assistant.py
Restart=always
RestartSec=3
Environment=NANOBOT_HOST=127.0.0.1

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now voice-assistant
```

### 4. Usage

1. Power on the Atom Echo, wait for the LED to turn blue-cyan (connected to server)
2. Press and hold the button (LED turns red), speak into the microphone
3. Release the button and wait for processing:
   - Orange → Speech recognition in progress
   - Yellow → STT complete, waiting for LLM
   - Purple → LLM thinking (with notification sound)
   - Cyan → Playing voice response
4. After playback, LED returns to blue-cyan, ready for next conversation

## LED Status Indicator

| Color | Status |
|-------|--------|
| Yellow | Starting up |
| Green | Connecting to WiFi |
| White | WiFi connected / WS reconnecting |
| Blue-Cyan | Ready / Idle |
| Red | Recording |
| Orange | Processing |
| Purple | LLM thinking |
| Cyan | Playing voice response |

## Key Configuration Parameters

| Parameter | ESP32 | Server | Description |
|-----------|-------|--------|-------------|
| Recording sample rate | 24kHz→16kHz | 16kHz | ESP32 internal downsampling |
| Playback sample rate | 24kHz | 24kHz | Must match on both sides |
| Opus frame duration | 60ms | 60ms | Must match on both sides |
| Opus bitrate | 24kbps | — | Set by encoder |
| WebSocket port | connect :8765 | listen :8765 | — |
| Codec volume | 95 | — | SetOutputVolume |
| TTS gain | — | +12dB | pydub |

## NanoBot / LLM Backend

This project uses [NanoBot](https://github.com/ArcInTower/NanoBot) as the LLM backend, streaming conversations over WebSocket. NanoBot provides thinking / tool_call / tool_result / final streaming events, allowing the device to show real-time processing progress.

If you don't use NanoBot, you can modify the `llm_stream()` method in `voice_assistant.py` to use any LLM API.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NANOBOT_HOST` | `127.0.0.1` | NanoBot server address |

## Technical Details

For more technical details, see [ARCHITECTURE.md](ARCHITECTURE.md), including:

- ESP32 firmware FreeRTOS task and queue architecture
- Opus codec configuration and direct API usage
- Playback pipeline 500ms silence padding strategy
- Server-side TTS Prefill + Pacing streaming strategy
- Pipeline statistics and zero frame loss verification
- Complete troubleshooting history

## AI Services

This project uses [SiliconFlow](https://cloud.siliconflow.cn/i/vnNqBaFz) AI inference services:

| Service | Model | Description |
|---------|-------|-------------|
| STT (Speech-to-Text) | SenseVoiceSmall | High-accuracy Chinese speech recognition |
| TTS (Text-to-Speech) | CosyVoice2-0.5B | Natural and fluent Chinese speech synthesis |

### Sponsored by SiliconFlow

<a href="https://cloud.siliconflow.cn/i/vnNqBaFz" target="_blank">
  <img src="https://cloud.siliconflow.cn/img/logo.png" alt="SiliconFlow" width="200">
</a>

The STT and TTS services in this project are powered by **[SiliconFlow](https://cloud.siliconflow.cn/i/vnNqBaFz)**. SiliconFlow provides high-performance, cost-effective AI inference services with instant access to a wide range of open-source models.

Sign up with the referral link below to get free credits:

**[https://cloud.siliconflow.cn/i/vnNqBaFz](https://cloud.siliconflow.cn/i/vnNqBaFz)**

Why SiliconFlow:
- High-performance inference acceleration with fast response times
- Supports SenseVoice, CosyVoice, Qwen, and many other open-source models
- Pay-as-you-go pricing with free credits for new users
- OpenAI-compatible API format for easy migration

## License

MIT
