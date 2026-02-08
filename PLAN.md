# Voice Assistant - Project Plan

## Current Architecture (v1 - Implemented)

```
[Atom Echo] --button press--> Record 16kHz mono via ES8311
[Atom Echo] --UDP:5000-----> [Mac: voice_assistant.py]
                              ├── STT: SenseVoiceSmall (SiliconFlow)
                              ├── LLM: DeepSeek-V3 (SiliconFlow)
                              └── TTS: CosyVoice2 (SiliconFlow)
[Atom Echo] <--UDP:5001----- [Mac: streaming PCM audio]
[Atom Echo] --ring buffer--> FreeRTOS playback task --> I2S --> ES8311 --> Speaker
```

## Next Steps (v2)

### 1. Replace LLM with OpenClaw
- Swap DeepSeek-V3 for OpenClaw as the conversational LLM backend
- Update `voice_assistant.py` LLM endpoint and payload format
- May need to adjust system prompt and response handling

### 2. Multi-WiFi Support
- Allow ESP32 to connect to multiple WiFi networks
- Use `WiFiMulti` library to try multiple SSIDs on boot
- Store WiFi credentials list (either hardcoded or in NVS/SPIFFS)
- Fallback mechanism: try next SSID if current fails

## Backlog

### Audio Quality
- [ ] Fix chime first-note attenuation (investigate ES8311 automute registers)
- [ ] Consider forking M5Atomic-EchoBase library to permanently fix 32-bit I2S
- [ ] Tune ring buffer size and initial buffer threshold for less stuttering

### Features
- [ ] Voice Activity Detection (VAD) instead of push-to-talk
- [ ] Conversation history / multi-turn support
- [ ] OTA firmware updates
- [ ] Status feedback via TTS (e.g., "WiFi connected", "Error")
