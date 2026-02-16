# Atom Echo Voice Assistant — 技术架构文档

## 1. 系统总览

```
┌─────────────────────────────────────────────────────────────────┐
│                    Server (Raspberry Pi / PC)                    │
│                                                                 │
│  ┌─────────────────────┐    ┌─────────────────────────────┐    │
│  │  voice_assistant.py  │    │  NanoBot (LLM Agent)        │    │
│  │  (aiohttp WS :8765) │◄──►│  (WS :18790/ws/chat)        │    │
│  │                      │    │  Claude + MCP tools          │    │
│  │  STT ──► LLM ──► TTS│    └─────────────────────────────┘    │
│  └──────────┬───────────┘                                       │
│             │ WebSocket (binary: Opus, text: JSON)              │
└─────────────┼───────────────────────────────────────────────────┘
              │ WiFi
┌─────────────┴───────────────────────────────────────────────────┐
│                 M5Stack Atom Echo + Echo Base                    │
│                                                                  │
│  Button ──► Mic ──► Opus Encode ──► WS Send                     │
│                                                                  │
│  WS Recv ──► Opus Decode ──► PCM Playback ──► Speaker           │
│                                                                  │
│  ESP32 (ESP-IDF 5.3.1 via PlatformIO)                           │
│  3 FreeRTOS Tasks: InputTask / CodecTask / OutputTask            │
└──────────────────────────────────────────────────────────────────┘
```

### 数据流

```
录音: Button Press → Mic (24kHz) → Downsample 16kHz → Opus Encode → WebSocket Binary
回放: WebSocket Binary → Opus Decode → PCM 24kHz → I2S DMA → ES8311 DAC → Speaker
```

### 通信协议 (ESP32 ↔ voice_assistant.py)

| 方向 | 类型 | 格式 | 说明 |
|------|------|------|------|
| ESP→Server | Binary | Opus packet | 麦克风音频帧 (16kHz, 60ms) |
| Server→ESP | Binary | Opus packet | TTS 音频帧 (24kHz, 60ms) |
| ESP→Server | Text | `{"type":"hello","audio":{...}}` | 设备上线 |
| ESP→Server | Text | `{"type":"record_start"}` | 按下按钮 |
| ESP→Server | Text | `{"type":"record_stop"}` | 松开按钮 |
| Server→ESP | Text | `{"type":"stt","text":"..."}` | 语音识别结果 |
| Server→ESP | Text | `{"type":"status","stage":"thinking\|tool_call\|tool_result"}` | LLM 处理状态 |
| Server→ESP | Text | `{"type":"tts_start"}` | TTS 开始播放 |
| Server→ESP | Text | `{"type":"tts_end"}` | TTS 播放结束 |

---

## 2. 硬件层 (Atom Echo + Echo Base)

### 芯片与外设

| 组件 | 型号 | 接口 | 说明 |
|------|------|------|------|
| SoC | ESP32-PICO-D4 | — | 双核 240MHz, 520KB SRAM, 4MB Flash |
| Audio Codec | ES8311 | I2C (0x18/0x30) + I2S | 24-bit ADC/DAC, 8~96kHz |
| I/O Expander | PI4IOE5V6408 | I2C (0x43) | 控制 Speaker Mute |
| Amplifier | NS4150B | — | 3W Class D, Echo Base 板载 |
| LED | SK6812 (RGBW) | GPIO27 via RMT | 单颗 LED 状态指示 |
| Button | — | GPIO39 (Active Low) | 内置按钮 |
| Microphone | SPM1423 | I2S DIN (GPIO23) | MEMS 麦克风 |

### GPIO 引脚映射

```
I2S:  BCK=33, WS=19, DOUT=22, DIN=23
I2C:  SDA=25, SCL=21
LED:  GPIO27 (SK6812, RMT 驱动, GRB 字节序)
BTN:  GPIO39 (Active Low, 内部无上拉, 外部上拉)
```

### I2C 地址说明

ES8311 的 7-bit 地址是 `0x18`，但 `esp_codec_dev` 要求传入 8-bit 格式的 `0x30`（它内部会右移一位）。如果你用 `i2c_master_probe` 扫描，能扫到的是 `0x18`。

### PI4IOE I/O Expander 初始化

```c
pi4ioe_write_reg(0x07, 0x00);  // Push-Pull mode
pi4ioe_write_reg(0x0D, 0xFF);  // Pull-up all
pi4ioe_write_reg(0x03, 0x6F);  // Direction
pi4ioe_write_reg(0x05, 0xFF);  // Output high (mute)
// Unmute: write_reg(0x05, 0xFF)
// Mute:   write_reg(0x05, 0x00)
```

### I2S 配置

```c
// 标准模式, 16-bit, 立体声 slot (ES8311 要求), 单声道数据
i2s_std_config_t:
  clk_cfg.sample_rate_hz = 24000
  clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256  // MCLK 从 BCK 派生
  slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT
  slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO       // ES8311 需要立体声 slot
  slot_cfg.bit_shift = true                        // I2S Philips 标准
  gpio_cfg.mclk = GPIO_NUM_NC                     // 不用外部 MCLK
```

### SK6812 LED 驱动 (RMT)

```c
// 使用 RMT bytes encoder, 10MHz 时钟 (100ns/tick)
bit0: 300ns high + 900ns low
bit1: 900ns high + 300ns low
// 数据顺序: GRB (不是 RGB!)
uint8_t grb[3] = {g, r, b};
rmt_transmit(channel, encoder, grb, 3, &tx_cfg);
```

---

## 3. ESP32 固件架构

### 文件结构

```
atom_echo_native/
├── platformio.ini          # PlatformIO 配置 (ESP-IDF 5.3.1)
├── partitions.csv          # 自定义分区表 (3MB app)
└── src/
    ├── main.cc             # 入口, 硬件初始化, WiFi, 按钮, LED, 通知音效
    ├── audio_codec.h       # 音频编解码器抽象基类
    ├── es8311_audio_codec.h/cc  # ES8311 具体实现 (esp_codec_dev)
    ├── audio_service.h/cc  # 核心音频管线 (3个FreeRTOS任务 + 4个队列)
    ├── ws_transport.h/cc   # WebSocket 传输层 (esp_websocket_client)
    └── audio_codec.cc      # AudioCodec 基类实现
```

### 分区表

```
nvs,      data, nvs,     0x9000,  0x6000   (24KB)
phy_init, data, phy,     0xf000,  0x1000   (4KB)
factory,  app,  factory, 0x10000, 0x300000  (3MB)
```

**为什么需要 3MB**: Opus 编解码库 (esp_opus_enc + esp_opus_dec) 编译后约 1.1MB，默认 1MB 分区放不下。

### PlatformIO 配置

```ini
[env:m5stack-atom]
platform = espressif32@6.9.0    # ESP-IDF 5.3.1
board = m5stack-atom
framework = espidf
board_build.partitions = partitions.csv
build_flags = -DCONFIG_LOG_DEFAULT_LEVEL=3
```

### 启动流程

```
app_main()
├── NVS Flash 初始化
├── LED 初始化 (RMT)  → 黄灯
├── I2C 初始化 + 设备探测 (ES8311 @ 0x18, PI4IOE @ 0x43)
├── PI4IOE 初始化 (speaker mute)
├── ES8311AudioCodec 创建 (I2S duplex, 24kHz)
│   └── codec->SetOutputVolume(95)
├── 播放启动音效 (play_chime: 700→1000→1400Hz 三音)
├── WiFi 连接 (多网络轮询)  → 绿灯
│   └── 等待 wifi_connected  → 白灯
├── AudioService 创建 + Start(24000)
│   ├── Opus 编码器初始化 (16kHz, mono, 24kbps, 60ms)
│   ├── Opus 解码器初始化 (24kHz, mono, 60ms)
│   ├── 创建 4 个 FreeRTOS 队列
│   └── 启动 3 个 FreeRTOS 任务
├── WsTransport 创建 + 注册回调
│   ├── SetAudioCallback: Opus → PushOpusForDecode
│   ├── SetJsonCallback: LED + 通知音效 + processing 锁
│   ├── SetDisconnectCallback: 重置 processing 状态
│   └── SetSendCallback: Opus → WS 发送
├── WebSocket 连接 (ws://SERVER_IP:8765)
│   └── 发送 hello JSON  → 蓝青灯
└── 主循环 (20ms tick)
    ├── WS 断连安全重置
    ├── 通知音效播放
    └── 按钮检测 (录音控制)
```

---

## 4. 核心: 音频管线 (AudioService)

这是最关键的部分。3 个 FreeRTOS 任务通过 4 个队列协作，实现双向 Opus 编解码。

### 任务与队列架构

```
                    ┌────────────────────────────────────────────┐
                    │           CodecTask (prio 2)               │
                    │           stack: 24KB                      │
                    │           heap buffers:                    │
                    │             enc_out: 4000B                 │
                    │             dec_out: 5760B                 │
                    │                                            │
  ┌──────────┐     │  ┌─────────┐         ┌──────────────┐      │     ┌──────────┐
  │InputTask │     │  │ Opus    │         │ Opus         │      │     │OutputTask│
  │(prio 8)  │────►│  │ Encode  │──send──►│ Decode       │──────│────►│(prio 4)  │
  │core 0    │ encode  │ 16kHz   │  cb     │ 24kHz        │ play │     │          │
  │stack:6KB │ _queue  │ 24kbps  │         │              │ back │     │stack:6KB │
  └──────────┘  (4) │  └─────────┘         └──────────────┘ _queue    └──────────┘
       ▲            │                                        (20) │        │
   Mic Read         │                           ▲                 │    Speaker Write
   24kHz→16kHz      └───────────────────────────┼─────────────────┘    + silence pad
   downsample                                   │
                                          decode_queue (30)
                                                ▲
                                                │
                                         PushOpusForDecode()
                                         (from WS callback)
```

### 队列规格

| 队列 | 深度 | 元素类型 | 元素大小 | 用途 |
|------|------|----------|----------|------|
| `encode_queue_` | 4 | `PcmBlock*` | 1920B (960 samples × 2B) | Mic PCM → Opus 编码 |
| `decode_queue_` | 30 | `OpusPacket*` | ≤512B | 服务器 Opus → 解码 |
| `playback_queue_` | 20 | `DecodedPcmBlock*` | ≤5760B (1440 samples × 2B) | 解码 PCM → 扬声器 |
| `send_queue_` | 10 | `OpusPacket*` | ≤512B | 编码 Opus → WS 发送 (未使用, 直接回调) |

**为什么深度这么设置:**
- `decode_queue_` = 30: 服务器用 prefill 策略会一次性发 10 帧, 需要足够深度不丢帧
- `playback_queue_` = 20: 缓冲 20×60ms = 1.2s 音频, 防止解码速度波动导致的欠载
- `encode_queue_` = 4: 录音实时性要求高, 不需要深缓冲

### Opus 编解码器配置

**编码器 (录音, ESP32 → Server):**
```c
esp_opus_enc_config_t:
  sample_rate = 16000     // 16kHz 对语音足够
  channel = 1             // 单声道
  bitrate = 24000         // 24kbps, 语音质量好且带宽低
  complexity = 0          // 最低复杂度, 减少 CPU 占用
  frame_duration = 60ms   // 每帧 960 samples
```

**解码器 (回放, Server → ESP32):**
```c
esp_opus_dec_cfg_t:
  sample_rate = 24000     // 24kHz 回放 (ES8311 运行在 24kHz)
  channel = 1             // 单声道
  // 每帧 1440 samples (24kHz × 60ms)
```

**关键**: 编码用 16kHz (节省带宽, STT 只需 16kHz), 解码用 24kHz (ES8311 运行在 24kHz, 避免重采样)。两端不对称但 Opus 支持这种配置。

### esp_opus 直接 API (不是 common API)

ESP-ADF 的 `esp_audio_enc_open()` (common API) 会链接所有编解码器, 导致固件膨胀。直接使用 `esp_opus_enc_open()` / `esp_opus_dec_open()` 只链接 Opus:

```c
// 编码
esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_encoder_);
esp_opus_enc_process(opus_encoder_, &in, &out);
esp_opus_enc_close(opus_encoder_);

// 解码
esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus_decoder_);
esp_opus_dec_decode(opus_decoder_, &raw, &out, &dec_info);
esp_opus_dec_close(opus_decoder_);
```

---

## 5. 回放管线详解 (最关键的部分)

从服务器发出 Opus 帧到扬声器播放, 经过 5 个阶段。每个阶段都有踩过的坑。

### 5.1 服务端 Opus 发送策略

```python
# voice_assistant.py: tts_and_stream()
PREFILL = 10       # 先发 10 帧填满 ESP32 缓冲 (~600ms)
FRAME_PACE = 0.055 # 之后每 55ms 发一帧 (略快于 60ms 实时)

for i, opus_pkt in enumerate(opus_frames):
    await self.ws.send_bytes(opus_pkt)
    if i >= PREFILL - 1 and i + 1 < len(opus_frames):
        await asyncio.sleep(FRAME_PACE)
```

**为什么需要 pacing:**
- 不 pacing: 服务器瞬间发完所有帧, ESP32 的 `decode_queue_` (深度 30) 可能溢出, `playback_queue_` (深度 20) 也可能溢出 → 丢帧
- 不 prefill: ESP32 播放队列空, 第一帧到达后立即播放, 但后续帧还在传输中 → 开头就有间断

**节奏计算:**
- 每帧 60ms 实时, pacing 55ms (稍快于实时)
- 预填 10 帧 = 600ms 缓冲 → 即使后续帧偶尔延迟也不会欠载
- 总延迟 ≈ 600ms (prefill) + TTS 生成时间

### 5.2 WebSocket 接收 → decode_queue

```c
// ws_transport.cc: WEBSOCKET_EVENT_DATA 回调
if (event->op_code == 0x02 && event->data_len > 0) {  // Binary
    on_audio_(event->data_ptr, event->data_len);
}

// audio_service.cc: PushOpusForDecode()
void AudioService::PushOpusForDecode(const uint8_t* data, size_t len) {
    auto* pkt = (OpusPacket*)malloc(sizeof(OpusPacket));
    memcpy(pkt->data, data, len);
    pkt->len = len;
    stat_rx_frames++;
    if (xQueueSend(decode_queue_, &pkt, 0) != pdTRUE) {
        stat_rx_dropped++;  // 队列满, 丢帧
        free(pkt);
    }
}
```

**WebSocket 缓冲**: `buffer_size = 8192`。最初用 4096 导致 8% 丢帧 (大的 Opus 包被截断)。

### 5.3 CodecTask: Opus 解码

```c
// CodecTask 主循环 (优先级 2, stack 24KB)
// 堆上分配大缓冲, 避免栈溢出:
auto* enc_out_buf = (uint8_t*)malloc(4000);   // 编码输出
auto* dec_out_buf = (int16_t*)malloc(5760);   // 解码输出 (1440 samples × 2B)

// 解码一帧:
esp_audio_dec_in_raw_t raw = { .buffer = opus_pkt->data, .len = opus_pkt->len };
esp_audio_dec_out_frame_t out = { .buffer = dec_out_buf, .len = 1440*2 };
esp_opus_dec_decode(opus_decoder_, &raw, &out, &dec_info);

// 解码成功 → 入 playback_queue_ (带 100ms 超时)
auto* pcm = (DecodedPcmBlock*)malloc(sizeof(DecodedPcmBlock));
memcpy(pcm->samples, dec_out_buf, out.decoded_size);
xQueueSend(playback_queue_, &pcm, pdMS_TO_TICKS(100));
```

**为什么 CodecTask 需要 24KB 栈**: Opus 解码器内部使用大量栈空间 (SILK/CELT 层), 8KB 不够。enc_out_buf 和 dec_out_buf 已经移到堆上, 但 Opus 库内部仍需要大量栈。

### 5.4 OutputTask: PCM 播放 (最复杂的部分)

```c
// OutputTask 主循环 (优先级 4, stack 6KB)
bool output_active = false;
int idle_ticks = 0;
const int MAX_IDLE_TICKS = 50;  // 500ms

while (running_) {
    DecodedPcmBlock* block = nullptr;

    if (xQueueReceive(playback_queue_, &block, pdMS_TO_TICKS(10))) {
        // ---- 有数据 ----
        if (!output_active) {
            codec_->EnableOutput(true);    // 打开 DAC + I2S TX
            output_active = true;
        }
        idle_ticks = 0;
        codec_->WriteSamples(block->samples, block->count);
        free(block);

    } else if (output_active) {
        // ---- 队列空, 写静音保持 I2S DMA 运转 ----
        int16_t silence[240] = {0};
        codec_->WriteSamples(silence, 240);  // 10ms 静音
        idle_ticks++;

        if (idle_ticks >= MAX_IDLE_TICKS) {
            // ---- 500ms 无数据, 关闭输出 ----
            // 先排空队列 (可能有残留帧)
            DecodedPcmBlock* drain;
            while (xQueueReceive(playback_queue_, &drain, 0) == pdTRUE) {
                codec_->WriteSamples(drain->samples, drain->count);
                free(drain);
            }
            codec_->EnableOutput(false);   // 关闭 DAC
            output_active = false;
        }

    } else {
        // ---- 输出未激活, 轻度休眠 ----
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

**为什么需要 500ms 静音填充 (这是最重要的设计决策):**

1. `EnableOutput(true/false)` 会调用 `esp_codec_dev_open/close`, 每次约 50ms
2. 如果 playback_queue_ 暂时空了 (解码速度波动), 立即关闭再打开会产生 50ms 间隙 → 音频断裂
3. 500ms 缓冲给了足够时间让 CodecTask 追上来
4. 写静音保持 I2S DMA 运转, 避免 PA 弹噪

**为什么关闭前要排空队列:**
- 在 500ms 等待期间, 可能有新帧到达
- 不排空会丢弃末尾 3~5 帧 → 语音尾巴被截断
- 验证: 修复后 `stat_played == stat_rx_frames` (零损失)

### 5.5 esp_codec_dev 写入 (底层)

```c
// es8311_audio_codec.cc
int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_ && dev_) {
        esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t));
    }
    return samples;
}
```

`esp_codec_dev_write` 是阻塞调用, 会等待 I2S DMA 缓冲区有空间。这提供了自然的节奏控制 — OutputTask 的写入速度被 I2S 采样率 (24kHz) 精确控制。

### I2S DMA 配置

```c
i2s_chan_config_t:
  dma_desc_num = 6          // 6 个 DMA 描述符
  dma_frame_num = 240       // 每个描述符 240 帧
  auto_clear_after_cb = true // DMA 完成后自动清零 (防止重复播放旧数据)
```

**DMA 缓冲总量**: 6 × 240 × 2B × 2ch = 5760B ≈ 120ms @ 24kHz (虽然是 STEREO slot, 实际只用单声道)

---

## 6. 录音管线

### InputTask: 麦克风 → PCM

```c
// InputTask (优先级 8, stack 6KB, 固定在 core 0)
// 以 10ms 为单位读取, 攒满 60ms 后打包

const int codec_sr = 24000;          // ES8311 运行在 24kHz
const int codec_frame = 24000 * 60 / 1000;  // = 1440 samples per 60ms
const int read_chunk = 24000 / 100;          // = 240 samples per 10ms

while (running_) {
    if (!recording_) { sleep(20ms); continue; }

    codec_->ReadSamples(read_buf + accumulated, 240);
    accumulated += 240;

    if (accumulated >= 1440) {
        // 24kHz → 16kHz 线性降采样
        float ratio = 24000.0f / 16000.0f;  // = 1.5
        for (int i = 0; i < 960; i++) {
            float src_idx = i * ratio;
            int idx = (int)src_idx;
            float frac = src_idx - idx;
            block->samples[i] = read_buf[idx] * (1-frac) + read_buf[idx+1] * frac;
        }
        // 960 samples @ 16kHz = 60ms → encode_queue_
        xQueueSend(encode_queue_, &block, 0);
        accumulated = 0;
    }
}
```

**为什么降采样**: ES8311 输入输出必须同采样率 (assert), 所以固定 24kHz。但 Opus 编码用 16kHz (STT 不需要更高, 且节省带宽)。

### CodecTask: Opus 编码 → 直接发送

```c
// 编码一帧 (960 samples @ 16kHz → Opus packet)
esp_opus_enc_process(opus_encoder_, &in, &out);

// 直接通过回调发送, 不经过 send_queue_
if (on_send_) {
    on_send_(enc_out_buf, out.encoded_bytes);
}
```

**编码后直接回调发送** (不用 send_queue_): 录音延迟越低越好, 额外的队列 hop 增加延迟。回调里直接调用 `ws->SendAudio()`, 发到 WebSocket。

### 启动录音的注意事项

```c
void AudioService::StartRecording() {
    codec_->EnableInput(true);
    vTaskDelay(pdMS_TO_TICKS(20));  // 等待 codec device 打开完成
    recording_ = true;              // 之后 InputTask 才开始读
}
```

**20ms 延迟**: `EnableInput(true)` 触发 `esp_codec_dev_open`, 异步完成。如果立即设 `recording_=true`, InputTask 会在 codec 还没准备好时调用 `esp_codec_dev_read`, 返回 `ESP_ERR_INVALID_STATE`。

---

## 7. 服务端架构 (voice_assistant.py)

### 处理流程

```
WebSocket Connected
  │
  ├── hello → 日志记录
  ├── record_start → recording=True, 清空 pcm_buffer
  ├── Binary msg → Opus decode (opuslib) → 追加到 pcm_buffer
  └── record_stop → recording=False → process_utterance()
         │
         ├── pcm_buffer → WAV (16kHz, 16-bit, mono)
         ├── STT (SiliconFlow SenseVoiceSmall)
         │     └── → send {"type":"stt","text":"..."}
         ├── LLM (NanoBot WebSocket streaming)
         │     ├── → send {"type":"status","stage":"thinking"}
         │     ├── → send {"type":"status","stage":"tool_call"}
         │     ├── → send {"type":"status","stage":"tool_result"}
         │     └── → 收到 "final" 事件, 取出 reply
         ├── clean_for_tts(reply) → 去除 Markdown
         ├── TTS (SiliconFlow CosyVoice2)
         │     └── WAV → PCM → +12dB gain → fade in/out → 100ms trailing silence
         ├── → send {"type":"tts_start"}
         ├── PCM → Opus encode (opuslib, 24kHz) → 分帧
         ├── Prefill 10 帧 → 55ms pacing 剩余帧
         └── → send {"type":"tts_end"}
```

### Opus 双解码器/编码器

```python
# 服务端需要两个 Opus 实例, 采样率不同:
self.opus_decoder = opuslib.Decoder(16000, 1)  # 解码 ESP32 发来的 16kHz 录音
self.opus_encoder = opuslib.Encoder(24000, 1, 'voip')  # 编码 24kHz TTS 回 ESP32
```

### TTS 后处理

```python
audio = AudioSegment.from_file(io.BytesIO(audio_data))
audio = audio.set_frame_rate(24000).set_channels(1).set_sample_width(2)
audio = audio + 12      # +12dB 增益 (CosyVoice 输出偏安静)
audio = audio.fade_in(30).fade_out(50)   # 淡入淡出防爆音
silence = AudioSegment.silent(duration=100)
audio = audio + silence  # 尾部 100ms 静音, 防止最后一个字被截断
```

**增益上限**: 14dB 开始失真, 12dB 是实测最佳值。20dB 完全失真且音量不再增加 (PA 限幅)。

### LLM 输出约束 (TTS 友好)

每次发给 LLM 的 prompt 前缀:
```
[语音输出模式] 你的回复将通过语音合成朗读给用户。请严格遵守：
1. 纯口语化表达，不要使用任何Markdown格式
2. 不要使用列表、编号、表格
3. 不要输出URL链接、文件路径、代码片段
4. 数字用口语读法
5. 简洁回复，控制在3-5句话以内
6. 不要使用括号注释
```

即便如此, LLM 有时仍会输出 Markdown, 所以还有 `clean_for_tts()` 做正则清洗 (去 `#`, `*`, `` ` ``, URL, 列表标记等)。

---

## 8. LED 状态机

| 颜色 | RGB | 状态 | 触发 |
|------|-----|------|------|
| 黄 | (20,20,0) | 启动中 | app_main 开始 |
| 绿 | (0,40,0) | WiFi 连接中 | wifi_init() |
| 白 | (20,20,20) | WiFi 已连接 / WS 断连 | wifi_connected / WS disconnect |
| 蓝青 | (0,20,40) | 就绪 / 空闲 | WS connected / tts_end |
| 红 | (60,0,0) | 录音中 | button press |
| 橙 | (60,30,0) | 处理中 (等待 STT) | button release |
| 黄 | (40,40,0) | STT 完成, 等待 LLM | "stt" 消息 |
| 紫 | (40,0,40) | LLM 思考中 | "thinking" 状态 |
| 橙 | (40,20,0) | 工具调用中 | "tool_call" 状态 |
| 黄绿 | (20,40,0) | 工具完成 | "tool_result" 状态 |
| 青 | (0,40,40) | TTS 播放中 | "tts_start" 消息 |

---

## 9. 通知音效

通过直接向 codec 写入正弦波 PCM 数据实现 (不经过 AudioService 管线)。

```c
// 在 main loop 中, 当 pending_notification > 0 时:
// 1. 打开 codec output (如果还没开)
// 2. 播放对应音效
// 3. 如果 processing 结束且无新通知 → 关闭 output

thinking:    350Hz 120ms + 60ms silence + 420Hz 120ms  // 低音双"噗噗"
tool_call:   520Hz 100ms + 50ms silence + 520Hz 100ms  // 中音双"嘟嘟"
tool_result: 500→800Hz 200ms sweep                      // 上升"叮~"
```

所有音效都有 25% fade in/out 包络, amplitude 2500 (ESP32 int16 范围 ±32767)。

**通知与 TTS 的协调:**
- `tts_start` 到来时: 清除 pending_notification, 设置 close_notif_output 标志
- main loop 检测到 close_notif_output → 关闭通知 output → OutputTask 重新打开 output 播放 TTS
- 这避免了两个 output 竞争 codec device

---

## 10. 状态管理与安全重置

### processing 锁

```c
static volatile bool processing = false;
// "stt" → processing = true  (禁止录音)
// "tts_end" → processing = false  (允许录音)
// WS disconnect → processing = false  (安全重置)
```

**防卡死**: 如果 WS 断连时 `processing=true`, main loop 检测到并自动重置:
```c
if (processing && !ws->IsConnected()) {
    processing = false;
    pending_notification = 0;
    close_notif_output = false;
    led_set(0, 20, 40);
}
```

还有 `SetDisconnectCallback` 在 WS 事件层立即重置。双重保险。

### WiFi 多网络轮询

```c
static const WiFiCredential wifi_list[] = {
    {"your_ssid_1",  "your_password_1"},
    {"your_ssid_2",  "your_password_2"},
};
// 每个网络重试 3 次后换下一个, 循环
// 断连后 500ms 延迟再重试
```

---

## 11. Pipeline 统计 (调试用)

```
STATS: rx=51 rx_drop=0 dec=51 dec_err=0 pb_q=51 pb_drop=0 played=51
```

| 计数器 | 含义 | 理想值 |
|--------|------|--------|
| rx | WS 收到的 Opus 帧数 | = server 发送数 |
| rx_drop | decode_queue_ 满丢弃 | 0 |
| dec | 成功解码帧数 | = rx |
| dec_err | 解码错误 | 0 |
| pb_q | 入 playback_queue_ | = dec |
| pb_drop | playback_queue_ 满丢弃 | 0 |
| played | 实际写入 codec | = pb_q |

**理想结果**: `rx == dec == pb_q == played`, 所有 drop/err = 0。

统计在 OutputTask 关闭 output 时打印并重置, 每个 playback session 独立计数。

---

## 12. 踩坑记录

| 问题 | 根因 | 解决方案 |
|------|------|----------|
| Flash 溢出 (108%) | Opus 库 ~1.1MB, 默认 1MB 分区 | 自定义 3MB app 分区 |
| esp_audio_enc_open 返回 -9 | common API 需要注册所有 codec | 用 esp_opus_enc_open 直接 API |
| Opus encode 返回 -5 | 输出缓冲 512B < encoder 要求 3840B | OPUS_ENC_OUTBUF_SIZE=4000 |
| 按按钮崩溃 | enc_out[4000]+dec_out[5760] 放栈上溢出 | malloc 堆分配 |
| esp_codec_dev_read INVALID_STATE | 录音标志设置太早, codec 没开完 | EnableInput 后等 20ms |
| 回放 2-3 秒后断裂 | OutputTask 频繁 EnableOutput on/off, 每次 ~50ms | 500ms 静音填充缓冲 |
| 服务端一次性发完导致溢出 | decode_queue/playback_queue 深度不够 | prefill 10 帧 + 55ms pacing |
| 8% WebSocket 帧丢失 | esp_websocket_client buffer=4096 太小 | 增加到 8192 |
| 播放末尾丢 5 帧 | OutputTask 关闭前不排空 playback_queue_ | 关闭前 drain 队列 |
| 音量太小 | CosyVoice 输出安静 | 服务端 +12dB, codec volume 95 |
| 14dB 失真 | PA 限幅 | 降回 12dB |
| LLM 输出 Markdown | 默认 LLM 行为 | VOICE_OUTPUT_PREFIX + clean_for_tts |
| opuslib macOS 找不到 libopus | Homebrew 不在 ctypes 搜索路径 | 条件性 monkey-patch find_library |
| Python 3.13 缺 audioop | 3.13 移除了 audioop 模块 | pip install audioop-lts |
| WS 断连后按钮卡死 | processing=true 没被重置 | DisconnectCallback + main loop 安全检查 |

---

## 13. 部署

### ESP32 固件

```bash
# 构建
pio run -e m5stack-atom -d atom_echo_native

# 刷写 (macOS USB serial)
pio run -e m5stack-atom -t upload \
  --upload-port /dev/cu.usbserial-XXXXX -d atom_echo_native

# 串口监控
pio device monitor --port /dev/cu.usbserial-XXXXX --baud 115200
```

### Raspberry Pi 服务端

```bash
# 系统依赖
sudo apt install -y libopus-dev ffmpeg

# Python 依赖
pip3 install aiohttp pydub pyyaml opuslib audioop-lts

# 配置
# secrets.yaml: siliconflow_api_key

# Systemd 服务
# /etc/systemd/system/voice-assistant.service
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
```

### 关键配置参数速查

| 参数 | ESP32 | Server | 说明 |
|------|-------|--------|------|
| 采样率 (录音) | 24kHz→16kHz | 16kHz | ESP32 降采样 |
| 采样率 (回放) | 24kHz | 24kHz | 必须一致 |
| Opus 帧长 | 60ms | 60ms | 必须一致 |
| Opus 比特率 | 24kbps | — | 编码端设定 |
| WS 端口 | 连接 :8765 | 监听 :8765 | — |
| codec volume | 95 | — | SetOutputVolume |
| TTS gain | — | +12dB | pydub |
| WS buffer | 8192 | — | esp_websocket_client |
| Prefill | — | 10 帧 | TTS 发送策略 |
| Pacing | — | 55ms/帧 | TTS 发送策略 |
