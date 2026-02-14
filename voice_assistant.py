"""
WebSocket-based Voice Assistant backend for Atom Echo (Opus transport).

Protocol (ESP32 ↔ this server):
  - Binary WebSocket messages = Opus-encoded audio frames
  - Text WebSocket messages = JSON control messages
  - ESP32 sends: {"type":"hello",...}, {"type":"record_start"}, {"type":"record_stop"}
  - Server sends: {"type":"tts_start"}, {"type":"tts_end"}, {"type":"stt","text":"..."}
  - Server sends: {"type":"status","stage":"thinking|tool_call|tool_result","detail":"..."}

LLM backend: NanoBot WebSocket streaming API at ws://192.168.31.165:18790/ws/chat
  Events: thinking → tool_call → tool_result → ... → done → final
"""

import asyncio
import ctypes
import ctypes.util
import io
import json
import logging
import os
import re
import struct
import time
import wave

import aiohttp
from aiohttp import web
from pydub import AudioSegment
import yaml

# --- Patch ctypes.util.find_library before importing opuslib ---
# opuslib calls find_library('opus') at import time; on macOS with Homebrew
# the system find_library doesn't search /opt/homebrew/lib.
# On Linux (Raspberry Pi) with libopus-dev installed, find_library works natively.
import platform
if platform.system() == "Darwin":
    _orig_find_library = ctypes.util.find_library
    def _patched_find_library(name):
        result = _orig_find_library(name)
        if result is None and name == 'opus':
            for p in ['/opt/homebrew/lib/libopus.dylib', '/usr/local/lib/libopus.dylib']:
                if os.path.exists(p):
                    return p
        return result
    ctypes.util.find_library = _patched_find_library

import opuslib

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# --- Config ---
secrets_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "secrets.yaml")
with open(secrets_path, "r") as f:
    secrets = yaml.safe_load(f)

WS_PORT = 8765
SILICONFLOW_API_KEY = secrets.get("siliconflow_api_key")
SILICONFLOW_BASE_URL = "https://api.siliconflow.cn/v1"

# NanoBot streaming WebSocket endpoint
# When running on the same host as NanoBot (e.g. Raspberry Pi), use localhost
NANOBOT_HOST = os.environ.get("NANOBOT_HOST", "192.168.31.165")
NANOBOT_WS_URL = f"ws://{NANOBOT_HOST}:18790/ws/chat"
NANOBOT_SESSION = "iot:device1"

# Prefix injected before every user message to constrain LLM output for TTS
VOICE_OUTPUT_PREFIX = (
    "[语音输出模式] 你的回复将通过语音合成朗读给用户。请严格遵守：\n"
    "1. 纯口语化表达，不要使用任何Markdown格式（不要#、*、-、```等符号）\n"
    "2. 不要使用列表、编号、表格，用自然的口语连接词代替（比如\"首先...然后...最后\"）\n"
    "3. 不要输出URL链接、文件路径、代码片段\n"
    "4. 数字用口语读法（比如\"一百二十三\"而不是\"123\"）\n"
    "5. 简洁回复，控制在3-5句话以内，避免长篇大论\n"
    "6. 不要使用括号注释（比如不要写\"CosyVoice（语音合成模型）\"）\n"
    "\n用户说："
)

# Opus config (must match ESP32)
OPUS_ENCODE_RATE = 16000   # mic recording rate for STT
OPUS_DECODE_RATE = 24000   # TTS playback rate on ESP32
OPUS_FRAME_MS = 60         # frame duration in ms
OPUS_CHANNELS = 1

STT_MODEL = "FunAudioLLM/SenseVoiceSmall"
TTS_MODEL = "FunAudioLLM/CosyVoice2-0.5B"
TTS_VOICE = "FunAudioLLM/CosyVoice2-0.5B:anna"


class VoiceSession:
    """Handles one WebSocket connection from an ESP32 device."""

    def __init__(self, ws: web.WebSocketResponse):
        self.ws = ws
        self.opus_decoder = opuslib.Decoder(OPUS_ENCODE_RATE, OPUS_CHANNELS)
        self.opus_encoder = opuslib.Encoder(OPUS_DECODE_RATE, OPUS_CHANNELS, 'voip')
        self.recording = False
        self.pcm_buffer = bytearray()
        self.processing = False

    async def handle_message(self, msg: aiohttp.WSMessage):
        if msg.type == aiohttp.WSMsgType.BINARY:
            await self.handle_audio(msg.data)
        elif msg.type == aiohttp.WSMsgType.TEXT:
            await self.handle_json(msg.data)

    async def handle_json(self, text: str):
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            logger.warning(f"Invalid JSON: {text[:100]}")
            return

        msg_type = data.get("type")
        if msg_type == "hello":
            logger.info(f"Device hello: {data}")
        elif msg_type == "record_start":
            logger.info("Recording started")
            self.recording = True
            self.pcm_buffer = bytearray()
        elif msg_type == "record_stop":
            logger.info(f"Recording stopped, buffer: {len(self.pcm_buffer)} bytes")
            self.recording = False
            if not self.processing:
                asyncio.create_task(self.process_utterance())

    async def handle_audio(self, opus_data: bytes):
        if not self.recording:
            return
        try:
            # Decode Opus to 16kHz PCM (matching encoder rate)
            frame_size = OPUS_ENCODE_RATE * OPUS_FRAME_MS // 1000  # 960 samples
            pcm = self.opus_decoder.decode(opus_data, frame_size)
            self.pcm_buffer.extend(pcm)
        except opuslib.OpusError as e:
            logger.warning(f"Opus decode error: {e}")

    async def process_utterance(self):
        if len(self.pcm_buffer) < 3200:  # < 100ms
            logger.info("Audio too short, ignoring")
            return

        self.processing = True
        try:
            # PCM buffer is 16kHz 16-bit mono
            wav_data = pcm_to_wav(self.pcm_buffer, OPUS_ENCODE_RATE)

            # STT
            text = await stt(wav_data)
            if not text:
                logger.warning("STT returned empty")
                self.processing = False
                return

            logger.info(f">> {text}")
            await self.send_json({"type": "stt", "text": text})

            # LLM via NanoBot streaming WebSocket
            t0 = time.time()
            reply = await self.llm_stream(text)
            t_llm = time.time() - t0
            logger.info(f"<< {reply} ({t_llm:.1f}s)")

            # TTS → Opus → stream to ESP32
            await self.tts_and_stream(reply)

        except Exception as e:
            logger.error(f"Process error: {e}", exc_info=True)
        finally:
            self.processing = False

    async def llm_stream(self, text: str) -> str:
        """Call NanoBot via WebSocket streaming API.
        Forwards intermediate events (thinking, tool_call, tool_result) to ESP32
        as status messages so the device can show progress on LED.
        Returns the final LLM response text.
        """
        voiced_message = VOICE_OUTPUT_PREFIX + text
        payload = json.dumps({"message": voiced_message, "session": NANOBOT_SESSION})
        reply = ""

        timeout = aiohttp.ClientTimeout(total=120)
        try:
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.ws_connect(NANOBOT_WS_URL) as nanobot_ws:
                    # Send the query
                    await nanobot_ws.send_str(payload)

                    # Process streaming events
                    async for msg in nanobot_ws:
                        if msg.type == aiohttp.WSMsgType.TEXT:
                            event = json.loads(msg.data)
                            evt_type = event.get("type")

                            if evt_type == "thinking":
                                iteration = event.get("iteration", 0)
                                logger.info(f"  [thinking] iteration {iteration}")
                                await self.send_json({
                                    "type": "status",
                                    "stage": "thinking",
                                    "detail": f"iteration {iteration}",
                                })

                            elif evt_type == "tool_call":
                                tool_name = event.get("name", "")
                                logger.info(f"  [tool_call] {tool_name}")
                                await self.send_json({
                                    "type": "status",
                                    "stage": "tool_call",
                                    "detail": tool_name,
                                })

                            elif evt_type == "tool_result":
                                tool_name = event.get("name", "")
                                result_preview = event.get("result", "")[:100]
                                logger.info(f"  [tool_result] {tool_name}: {result_preview}")
                                await self.send_json({
                                    "type": "status",
                                    "stage": "tool_result",
                                    "detail": tool_name,
                                })

                            elif evt_type == "final":
                                reply = event.get("content", "").strip()
                                break  # Terminal event

                            elif evt_type == "done":
                                # done arrives just before final; use final as authoritative
                                pass

                        elif msg.type in (aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSED):
                            logger.error(f"NanoBot WS error/closed")
                            break

        except Exception as e:
            logger.error(f"NanoBot WS error: {e}", exc_info=True)

        if not reply:
            reply = "抱歉，我没有得到回复。"
        return clean_for_tts(reply)

    async def tts_and_stream(self, text: str):
        await self.send_json({"type": "tts_start"})

        try:
            pcm_data = await tts_to_pcm(text, OPUS_DECODE_RATE)
            if not pcm_data:
                logger.error("TTS failed")
                await self.send_json({"type": "tts_end"})
                return

            # Encode PCM to Opus frames and send
            frame_samples = OPUS_DECODE_RATE * OPUS_FRAME_MS // 1000  # 1440 samples @ 24kHz
            frame_bytes = frame_samples * 2  # 16-bit
            offset = 0
            frame_count = 0

            # Pre-encode all frames first, then stream with real-time pacing
            opus_frames = []
            while offset + frame_bytes <= len(pcm_data):
                frame = pcm_data[offset:offset + frame_bytes]
                opus_pkt = self.opus_encoder.encode(frame, frame_samples)
                opus_frames.append(opus_pkt)
                offset += frame_bytes

            # Stream with real-time pacing to avoid playback queue underrun.
            # Send first burst of 10 frames to pre-fill buffer (~600ms),
            # then pace 1 frame per ~55ms (slightly faster than 60ms real-time).
            PREFILL = 10  # frames to send immediately to fill ESP32 buffer
            FRAME_PACE = 0.055  # seconds between frames after prefill

            for i, opus_pkt in enumerate(opus_frames):
                await self.ws.send_bytes(opus_pkt)
                frame_count += 1
                if i >= PREFILL - 1 and i + 1 < len(opus_frames):
                    await asyncio.sleep(FRAME_PACE)

            logger.info(f"Streamed {frame_count} Opus frames ({len(opus_frames) * OPUS_FRAME_MS / 1000:.1f}s)")

        except Exception as e:
            logger.error(f"TTS stream error: {e}", exc_info=True)

        await self.send_json({"type": "tts_end"})

    async def send_json(self, data: dict):
        try:
            await self.ws.send_str(json.dumps(data))
        except Exception:
            pass


# --- Shared utilities ---

def clean_for_tts(text: str) -> str:
    """Clean LLM output for natural TTS reading."""
    # Remove markdown headers
    text = re.sub(r'#{1,6}\s*', '', text)
    # Remove bold/italic markers
    text = re.sub(r'\*{1,3}([^*]+)\*{1,3}', r'\1', text)
    # Remove inline code backticks
    text = re.sub(r'`([^`]+)`', r'\1', text)
    # Remove code blocks
    text = re.sub(r'```[\s\S]*?```', '', text)
    # Remove markdown links, keep text: [text](url) → text
    text = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text)
    # Remove bare URLs
    text = re.sub(r'https?://\S+', '', text)
    # Remove list markers (-, *, numbered)
    text = re.sub(r'^\s*[-*]\s+', '', text, flags=re.MULTILINE)
    text = re.sub(r'^\s*\d+[.)]\s+', '', text, flags=re.MULTILINE)
    # Remove excessive whitespace/newlines, normalize to single spaces
    text = re.sub(r'\n{2,}', '。', text)  # paragraph breaks → period
    text = re.sub(r'\n', '，', text)      # line breaks → comma
    text = re.sub(r'\s{2,}', ' ', text)
    # Remove remaining special chars that TTS might read literally
    text = re.sub(r'[<>{}|\\~^]', '', text)
    return text.strip()


def pcm_to_wav(pcm_data: bytes, sample_rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(sample_rate)
        f.writeframes(pcm_data)
    return buf.getvalue()


async def stt(wav_data: bytes) -> str:
    headers = {"Authorization": f"Bearer {SILICONFLOW_API_KEY}"}
    data = aiohttp.FormData()
    data.add_field('file', wav_data, filename='audio.wav', content_type='audio/wav')
    data.add_field('model', STT_MODEL)
    async with aiohttp.ClientSession() as session:
        try:
            async with session.post(
                f"{SILICONFLOW_BASE_URL}/audio/transcriptions",
                data=data, headers=headers
            ) as resp:
                if resp.status == 200:
                    res = await resp.json()
                    return res.get('text', '')
                else:
                    body = await resp.text()
                    logger.error(f"STT error {resp.status}: {body[:200]}")
        except Exception as e:
            logger.error(f"STT exception: {e}")
    return ""


async def tts_to_pcm(text: str, target_rate: int) -> bytes:
    headers = {
        "Authorization": f"Bearer {SILICONFLOW_API_KEY}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": TTS_MODEL,
        "input": text,
        "voice": TTS_VOICE,
        "response_format": "wav",
        "sample_rate": target_rate,
    }
    async with aiohttp.ClientSession() as session:
        try:
            async with session.post(
                f"{SILICONFLOW_BASE_URL}/audio/speech",
                json=payload, headers=headers
            ) as resp:
                if resp.status == 200:
                    audio_data = await resp.read()
                    audio = AudioSegment.from_file(io.BytesIO(audio_data))
                    audio = audio.set_frame_rate(target_rate).set_channels(1).set_sample_width(2)
                    audio = audio + 12  # gain (dB)
                    audio = audio.fade_in(30).fade_out(50)
                    silence = AudioSegment.silent(duration=100, frame_rate=target_rate)
                    audio = audio + silence
                    return audio.raw_data
                else:
                    body = await resp.text()
                    logger.error(f"TTS error {resp.status}: {body[:200]}")
        except Exception as e:
            logger.error(f"TTS exception: {e}")
    return b""


# --- WebSocket handler ---

async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    logger.info(f"Client connected: {request.remote}")

    session = VoiceSession(ws)

    try:
        async for msg in ws:
            if msg.type in (aiohttp.WSMsgType.BINARY, aiohttp.WSMsgType.TEXT):
                await session.handle_message(msg)
            elif msg.type == aiohttp.WSMsgType.ERROR:
                logger.error(f"WS error: {ws.exception()}")
    except Exception as e:
        logger.error(f"WS handler error: {e}")
    finally:
        logger.info("Client disconnected")

    return ws


async def main():
    app = web.Application()
    app.router.add_get('/ws', websocket_handler)
    app.router.add_get('/', websocket_handler)  # ESP32 connects to root

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', WS_PORT)
    await site.start()
    logger.info(f"WebSocket server listening on ws://0.0.0.0:{WS_PORT}")
    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
