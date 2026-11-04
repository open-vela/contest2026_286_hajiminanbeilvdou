#!/usr/bin/env python3
"""Capture raw device audio for N seconds, with no VAD or PTT gating.

This is a diagnostic tool: it proves whether the microphone/link delivers a
complete sentence before the higher layers (PTT window, VAD, wake phrase) get
a chance to cut it.  The WAV is saved untouched at the device rate.

    ~/voiceenv/bin/python record_raw.py --seconds 60 --out /tmp/raw.wav
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import wave

import websockets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_bridge as vb


async def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="192.168.223.2")
    p.add_argument("--port", type=int, default=28789)
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--out", default="/tmp/raw_capture.wav")
    args = p.parse_args()

    url = "ws://%s:%d" % (args.host, args.port)
    async with websockets.connect(url, max_size=None,
                                  ping_interval=None) as ws:
        # Stop any stream the live bridge left behind, then own the mic.
        await ws.send(json.dumps({"type": "mic_stop"}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"type": "mic_start", "samples": 1024}))

        fmt = None
        codec = "ulaw"
        adpcm = vb.ImaAdpcmDecoder()
        pcm = bytearray()
        frames = 0
        t0 = time.time()
        print("RECORDING %.0fs -> %s" % (args.seconds, args.out), flush=True)

        while time.time() - t0 < args.seconds:
            try:
                m = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                print("no frame for 5s, stopping", flush=True)
                break
            if isinstance(m, bytes):
                frames += 1
                if codec == "ima_adpcm":
                    pcm += adpcm.decode(m)
                else:
                    pcm += vb.ulaw_decode(m).tobytes()
                continue
            try:
                d = json.loads(m)
            except ValueError:
                continue
            if d.get("type") == "audio_format":
                fmt = d
                codec = d.get("codec", "ulaw")
                print("format:", d, flush=True)

        try:
            await ws.send(json.dumps({"type": "mic_stop"}))
        except Exception:
            pass

    rate = int((fmt or {}).get("rate", vb.DEVICE_RATE))
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(pcm))
    dur = len(pcm) / 2.0 / rate
    print("saved %s: %d samples, %.2fs, %d frames"
          % (args.out, len(pcm) // 2, dur, frames), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
