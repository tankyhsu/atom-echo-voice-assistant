# M5Stack Atom Echo Voice Assistant - Setup Instructions

## Prerequisites

### Hardware
- M5Stack Atom Echo with Atomic Echo Base (SKU A149)
- USB-C cable

### Software (Mac)
- Python 3.9+
- PlatformIO CLI (`pip install platformio`)
- Python packages: `pydub`, `aiohttp`, `pyyaml`

### Accounts
- SiliconFlow API key (for STT/LLM/TTS)

## Setup

### 1. Configure Secrets

Edit `secrets.yaml` (not tracked by git):
```yaml
siliconflow_api_key: "sk-your-api-key-here"
```

### 2. Configure Network

In `atom_echo_native/src/main.cpp`, update:
```cpp
const char* ssid     = "your-wifi-ssid";
const char* password = "your-wifi-password";
const char* mac_ip   = "your-mac-ip-address";
```

### 3. Apply Library Patch

The M5Atomic-EchoBase library has a critical bug (16-bit I2S vs 32-bit codec). After first build or `pio lib install`:

Edit `.pio/libdeps/m5stack-atom/M5Atomic-EchoBase/src/M5EchoBase.cpp` line 110:
```cpp
// Change FROM:
i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
// Change TO:
i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
```

### 4. Build & Flash Firmware

```bash
cd atom_echo_native
pio run -t upload --upload-port /dev/cu.usbserial-XXXXX
```

### 5. Run Python Backend

```bash
python3 voice_assistant.py
```

## Usage

1. Device boots with ascending 3-note chime, then connects to WiFi (green breathing LED)
2. LED turns dim white when ready
3. **Press and hold** the button to record (LED turns red)
4. **Release** to send audio for processing (LED turns orange)
5. Response plays back automatically (LED turns white)

## Troubleshooting

- **No sound / harsh audio**: Check that the library patch (step 3) is applied
- **Audio cuts off**: Ensure Python backend is running and on the same network
- **First word unclear**: Known issue, PA warmup time. Will improve in future versions
