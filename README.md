# Atom Echo 语音助手

基于 M5Stack Atom Echo 的离线硬件 + 云端 AI 语音助手。按住按钮说话，松开后自动完成语音识别、LLM 推理、语音合成并播放回复。

完全使用 ESP-IDF 原生框架开发（非 Arduino），通过 Opus 编解码 + WebSocket 实现低延迟双向语音流。

## 功能特性

- **按键对讲**：按住按钮录音，松开后自动处理
- **全链路语音**：STT → LLM → TTS，端到端语音交互
- **Opus 音频流**：低带宽、高质量的实时音频传输
- **NanoBot 集成**：支持 LLM Agent 的 thinking / tool_call / tool_result 流式事件
- **LED 状态指示**：通过 SK6812 RGB LED 实时显示设备状态
- **通知音效**：LLM 思考、工具调用、工具完成各有独特提示音
- **多 WiFi 支持**：自动轮询多个 WiFi 网络
- **零帧丢失**：精心调优的音频管线，实测 rx == dec == pb_q == played
- **Docker 部署**：服务端支持 Docker 一键部署

## 系统架构

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

### 数据流

```
录音: Button Press → Mic (24kHz) → Downsample 16kHz → Opus Encode → WebSocket
回放: WebSocket → Opus Decode → PCM 24kHz → I2S DMA → ES8311 DAC → Speaker
```

### 通信协议 (ESP32 ↔ Server)

| 方向 | 类型 | 格式 | 说明 |
|------|------|------|------|
| ESP→Server | Binary | Opus packet | 麦克风音频帧 (16kHz, 60ms) |
| Server→ESP | Binary | Opus packet | TTS 音频帧 (24kHz, 60ms) |
| ESP→Server | Text | `{"type":"hello","audio":{...}}` | 设备上线 |
| ESP→Server | Text | `{"type":"record_start"}` | 按下按钮 |
| ESP→Server | Text | `{"type":"record_stop"}` | 松开按钮 |
| Server→ESP | Text | `{"type":"stt","text":"..."}` | 语音识别结果 |
| Server→ESP | Text | `{"type":"status","stage":"..."}` | LLM 处理状态 |
| Server→ESP | Text | `{"type":"tts_start"}` | TTS 开始播放 |
| Server→ESP | Text | `{"type":"tts_end"}` | TTS 播放结束 |

## 硬件需求

### 设备

- **M5Stack Atom Echo** — ESP32-PICO-D4 开发板，内置 MEMS 麦克风和按钮
- **M5Stack Echo Base** — 扩展底座，带 ES8311 音频编解码器 + NS4150B 功放 + 扬声器

### 芯片与外设

| 组件 | 型号 | 接口 | 说明 |
|------|------|------|------|
| SoC | ESP32-PICO-D4 | — | 双核 240MHz, 520KB SRAM, 4MB Flash |
| Audio Codec | ES8311 | I2C + I2S | 24-bit ADC/DAC |
| I/O Expander | PI4IOE5V6408 | I2C (0x43) | 控制扬声器静音 |
| Amplifier | NS4150B | — | 3W Class D |
| LED | SK6812 | GPIO27 (RMT) | RGB 状态指示 |
| Button | — | GPIO39 | Active Low |
| Microphone | SPM1423 | I2S | MEMS 麦克风 |

### 服务端

- Raspberry Pi 4/5 或任何 Linux/macOS 主机
- Python 3.9+
- libopus, ffmpeg

## 项目结构

```
.
├── atom_echo_native/              # ESP32 固件 (ESP-IDF)
│   ├── platformio.ini             # PlatformIO 配置
│   ├── partitions.csv             # 自定义分区表 (3MB app)
│   ├── sdkconfig.defaults         # ESP-IDF 默认配置
│   └── src/
│       ├── main.cc                # 入口, 硬件初始化, WiFi, 按钮, LED
│       ├── audio_codec.h/cc       # 音频编解码器抽象基类
│       ├── es8311_audio_codec.h/cc # ES8311 编解码器实现
│       ├── audio_service.h/cc     # 核心音频管线 (3 FreeRTOS 任务)
│       ├── ws_transport.h/cc      # WebSocket 传输层
│       └── idf_component.yml      # 组件依赖
├── voice_assistant.py             # 服务端 (Python)
├── requirements.txt               # Python 依赖
├── secrets.yaml.example           # 配置模板
├── Dockerfile                     # Docker 镜像
├── docker-compose.yml             # Docker Compose 部署
└── ARCHITECTURE.md                # 详细技术架构文档
```

## 快速开始

### 1. 准备配置

复制配置模板并填入你的信息：

```bash
cp secrets.yaml.example secrets.yaml
```

编辑 `secrets.yaml`：

```yaml
siliconflow_api_key: "sk-your-api-key-here"
```

你需要一个 [SiliconFlow](https://cloud.siliconflow.cn/i/vnNqBaFz) 的 API Key，用于 STT (SenseVoiceSmall) 和 TTS (CosyVoice2)。

### 2. 构建并刷写 ESP32 固件

#### 前置条件

- [PlatformIO CLI](https://platformio.org/install/cli) 已安装
- USB 线连接 Atom Echo

#### 修改固件配置

编辑 `atom_echo_native/src/main.cc`，修改以下内容：

```cpp
// WiFi 配置
static const WiFiCredential wifi_list[] = {
    {"your_wifi_ssid",  "your_wifi_password"},
};

// 服务端 IP（运行 voice_assistant.py 的机器）
#define BACKEND_IP "192.168.x.x"
```

#### 构建和刷写

```bash
# 构建
pio run -e m5stack-atom -d atom_echo_native

# 刷写（替换为你的串口设备）
pio run -e m5stack-atom -t upload \
  --upload-port /dev/cu.usbserial-XXXXX -d atom_echo_native

# 查看串口日志
pio device monitor --port /dev/cu.usbserial-XXXXX --baud 115200
```

### 3. 部署服务端

#### 方式一：直接运行

```bash
# 安装系统依赖
# macOS:
brew install opus ffmpeg

# Debian/Ubuntu (Raspberry Pi):
sudo apt install -y libopus-dev ffmpeg

# 安装 Python 依赖
pip3 install -r requirements.txt

# 运行
python3 voice_assistant.py
```

#### 方式二：Docker 部署

```bash
docker compose up -d
```

#### 方式三：Systemd 服务（推荐用于 Raspberry Pi）

```bash
sudo cp voice_assistant.py /opt/voice-assistant/
sudo cp secrets.yaml /opt/voice-assistant/
sudo cp requirements.txt /opt/voice-assistant/

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

### 4. 使用

1. 给 Atom Echo 上电，等待 LED 变为蓝青色（表示已连接服务端）
2. 按住按钮（LED 变红），对着麦克风说话
3. 松开按钮，等待处理：
   - 橙色 → 正在进行语音识别
   - 黄色 → STT 完成，等待 LLM
   - 紫色 → LLM 思考中（伴随提示音）
   - 青色 → 正在播放语音回复
4. 播放完成后 LED 恢复蓝青色，可以进行下一轮对话

## LED 状态指示

| 颜色 | 状态 |
|------|------|
| 黄色 | 启动中 |
| 绿色 | WiFi 连接中 |
| 白色 | WiFi 已连接 / WS 断连重连中 |
| 蓝青色 | 就绪 / 空闲 |
| 红色 | 录音中 |
| 橙色 | 正在处理 |
| 紫色 | LLM 思考中 |
| 青色 | 播放语音回复中 |

## 关键配置参数

| 参数 | ESP32 | Server | 说明 |
|------|-------|--------|------|
| 录音采样率 | 24kHz→16kHz | 16kHz | ESP32 内部降采样 |
| 回放采样率 | 24kHz | 24kHz | 两端必须一致 |
| Opus 帧长 | 60ms | 60ms | 两端必须一致 |
| Opus 比特率 | 24kbps | — | 编码端设定 |
| WebSocket 端口 | 连接 :8765 | 监听 :8765 | — |
| Codec 音量 | 95 | — | SetOutputVolume |
| TTS 增益 | — | +12dB | pydub |

## NanoBot / LLM 后端

本项目使用 [NanoBot](https://github.com/ArcInTower/NanoBot) 作为 LLM 后端，通过 WebSocket 实现流式对话。NanoBot 提供 thinking / tool_call / tool_result / final 等流式事件，设备端可实时显示处理进度。

如果你不使用 NanoBot，可以修改 `voice_assistant.py` 中的 `llm_stream()` 方法，替换为任意 LLM API。

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `NANOBOT_HOST` | `127.0.0.1` | NanoBot 服务地址 |

## 技术细节

更多技术细节请参考 [ARCHITECTURE.md](ARCHITECTURE.md)，包括：

- ESP32 固件 FreeRTOS 任务与队列架构
- Opus 编解码器配置与直接 API 使用
- 回放管线的 500ms 静音填充策略
- 服务端 TTS Prefill + Pacing 发送策略
- Pipeline 统计与零帧丢失验证
- 完整的踩坑记录

## AI 服务

本项目使用 [SiliconFlow](https://cloud.siliconflow.cn/i/vnNqBaFz) 提供的 AI 推理服务：

| 服务 | 模型 | 说明 |
|------|------|------|
| STT（语音识别） | SenseVoiceSmall | 高精度中文语音识别 |
| TTS（语音合成） | CosyVoice2-0.5B | 自然流畅的中文语音合成 |

### 推广链接

<a href="https://cloud.siliconflow.cn/i/vnNqBaFz" target="_blank">
  <img src="https://cloud.siliconflow.cn/img/logo.png" alt="SiliconFlow" width="200">
</a>

本项目的 STT 和 TTS 服务由 **[SiliconFlow 硅基流动](https://cloud.siliconflow.cn/i/vnNqBaFz)** 提供。SiliconFlow 提供高性能、低成本的 AI 推理服务，支持多种开源模型的即时调用。

通过以下推广链接或扫描二维码注册即可获得免费额度：

**[https://cloud.siliconflow.cn/i/vnNqBaFz](https://cloud.siliconflow.cn/i/vnNqBaFz)**

<a href="https://cloud.siliconflow.cn/i/vnNqBaFz" target="_blank">
  <img src="assets/siliconflow_qr.png" alt="SiliconFlow 注册二维码" width="200">
</a>

SiliconFlow 的优势：
- 高性能推理加速，响应速度快
- 支持 SenseVoice、CosyVoice、Qwen 等多种开源模型
- 按量计费，新用户有免费额度
- API 兼容 OpenAI 格式，迁移成本低

## License

MIT
