import asyncio
import socket
import logging
import io
import wave
import yaml
import aiohttp
import os
import time
from pydub import AudioSegment

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

secrets_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "secrets.yaml")
with open(secrets_path, "r") as f:
    secrets = yaml.safe_load(f)

UDP_IP = "0.0.0.0"
UDP_RX_PORT = 5000
UDP_TX_PORT = 5001
SILICONFLOW_API_KEY = secrets.get("siliconflow_api_key")
SILICONFLOW_BASE_URL = "https://api.siliconflow.cn/v1"

STT_MODEL = "FunAudioLLM/SenseVoiceSmall"
LLM_MODEL = "deepseek-ai/DeepSeek-V3"
TTS_MODEL = "FunAudioLLM/CosyVoice2-0.5B"
TTS_VOICE = "FunAudioLLM/CosyVoice2-0.5B:anna"

SYSTEM_PROMPT = "你是一个助手。回复极简。"

class VoiceAssistant:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((UDP_IP, UDP_RX_PORT))
        self.sock.settimeout(0.3) 
        self.audio_buffer = bytearray()
        self.client_addr = None 
        self.is_processing = False

    def pcm_to_wav(self, pcm_data):
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(16000)
            wav_file.writeframes(pcm_data)
        return buf.getvalue()

    async def stt(self, wav_data):
        headers = {"Authorization": f"Bearer {SILICONFLOW_API_KEY}"}
        data = aiohttp.FormData()
        data.add_field('file', wav_data, filename='audio.wav', content_type='audio/wav')
        data.add_field('model', STT_MODEL)
        async with aiohttp.ClientSession() as session:
            try:
                async with session.post(f"{SILICONFLOW_BASE_URL}/audio/transcriptions", data=data, headers=headers) as resp:
                    if resp.status == 200:
                        res = await resp.json()
                        return res.get('text', '')
            except: pass
        return None

    async def llm(self, text):
        headers = {"Authorization": f"Bearer {SILICONFLOW_API_KEY}", "Content-Type": "application/json"}
        payload = {
            "model": LLM_MODEL,
            "messages": [{"role": "system", "content": SYSTEM_PROMPT}, {"role": "user", "content": text}],
            "max_tokens": 64
        }
        async with aiohttp.ClientSession() as session:
            try:
                async with session.post(f"{SILICONFLOW_BASE_URL}/chat/completions", json=payload, headers=headers) as resp:
                    if resp.status == 200:
                        res = await resp.json()
                        return res['choices'][0]['message']['content']
            except: pass
        return "思考出错。"

    async def tts_download_and_stream(self, text):
        if not self.client_addr: return
        target_ip = self.client_addr[0]

        logger.info(f"Downloading Audio...")
        headers = {"Authorization": f"Bearer {SILICONFLOW_API_KEY}", "Content-Type": "application/json"}
        payload = {
            "model": TTS_MODEL,
            "input": text,
            "voice": TTS_VOICE,
            "response_format": "wav", 
            "sample_rate": 16000
        }
        
        async with aiohttp.ClientSession() as session:
             try:
                async with session.post(f"{SILICONFLOW_BASE_URL}/audio/speech", json=payload, headers=headers) as resp:
                    if resp.status == 200:
                        audio_data = await resp.read()
                        
                        # Convert to 16k Mono 16-bit PCM with fade to avoid pop
                        audio = AudioSegment.from_file(io.BytesIO(audio_data))
                        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
                        audio = audio + 6
                        audio = audio.fade_in(30).fade_out(30)
                        pcm_data = audio.raw_data

                        logger.info(f"Streaming Mono {len(pcm_data)} bytes ({len(pcm_data)/32000:.1f}s)...")

                        chunk_size = 1024
                        for i in range(0, len(pcm_data), chunk_size):
                            chunk = pcm_data[i:i+chunk_size]
                            self.sock.sendto(chunk, (target_ip, UDP_TX_PORT))
                            time.sleep(0.028)
                        
                        logger.info("Done.")
             except Exception as e:
                logger.error(f"TTS Exception: {e}")

    async def main_loop(self):
        logger.info("Ready. Mono streaming mode.")
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
                self.client_addr = addr
                if not self.is_processing:
                    self.audio_buffer.extend(data)
            except socket.timeout:
                if self.audio_buffer:
                    if len(self.audio_buffer) < 2000:
                        self.audio_buffer = bytearray()
                        continue

                    self.is_processing = True
                    buf = io.BytesIO()
                    with wave.open(buf, 'wb') as f:
                        f.setnchannels(1); f.setsampwidth(2); f.setframerate(16000)
                        f.writeframes(self.audio_buffer)
                    
                    text = await self.stt(buf.getvalue())
                    if text:
                        logger.info(f">> {text}")
                        reply = await self.llm(text)
                        logger.info(f"<< {reply}")
                        await self.tts_download_and_stream(reply)
                    
                    self.audio_buffer = bytearray()
                    self.is_processing = False
                continue

if __name__ == "__main__":
    va = VoiceAssistant()
    asyncio.run(va.main_loop())
