#!/usr/bin/env python3
"""Benchmark one sherpa-onnx ASR model on the TTS command set.

The device path is simulated by resampling to 8462 Hz and round-tripping
through G.711 mu-law; --noisy adds white noise before that, which is the
closest offline proxy for a desk microphone.

    ~/voiceenv/bin/python asr_bench.py --model firered-aed --threads 8 \
        --wav-dir /mnt/c/Users/.../tts_cmds
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import time
import wave

import numpy as np
import soxr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_bridge as vb

# What each generated TTS file is expected to route to.  Substring match
# against the canonical command returned by understand().
EXPECTED = {
    0: "现在几点了",
    1: "内存",
    2: "请每5分钟",
    3: "取消所有定时任务",
    4: "任务",
    5: "在屏幕上显示",
    6: "立即巡检",
    7: "介绍一下你自己",
    8: "请每10分钟",
    9: "立即巡检",
}


def ulaw_encode(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, -32768, 32767).astype(np.int32)
    sign = x < 0
    mag = np.minimum(np.abs(x), 32635) + 0x84
    exp = np.clip(np.floor(np.log2(mag)).astype(np.int32) - 7, 0, 7)
    mant = (mag >> (exp + 3)) & 0x0F
    return ((~((sign.astype(np.uint8) << 7) | (exp << 4) | mant)) & 0xFF) \
        .astype(np.uint8)


def read_wav(path: str) -> tuple[int, bytes]:
    with wave.open(path, "rb") as w:
        return w.getframerate(), w.readframes(w.getnframes())


def simulate_link(pcm: bytes, rate: int, noisy: bool = False,
                  snr_db: float = 10.0) -> bytes:
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if noisy:
        rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2))) + 1e-9
        sigma = rms / (10.0 ** (snr_db / 20.0))
        rng = np.random.default_rng(1234)
        x = x + rng.normal(0.0, sigma, x.shape).astype(np.float32)
    y = soxr.resample(x, rate, 8462)
    y = np.clip(np.round(y), -32768, 32767).astype(np.int16)
    return vb.ulaw_decode(ulaw_encode(y)).tobytes()


def route(text: str) -> str:
    wake, tail = vb.split_wake(text)
    return vb.understand(tail if wake else text)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--model", default="paraformer-small",
                   choices=sorted(vb.ASR_MODELS))
    p.add_argument("--threads", type=int, default=8)
    p.add_argument("--wav-dir", required=True)
    p.add_argument("--noisy", action="store_true",
                   help="also benchmark a noisy version")
    p.add_argument("--snr", type=float, default=10.0,
                   help="SNR in dB for the noisy pass")
    args = p.parse_args()

    missing = [f for f in vb.asr_model_files(args.model)
               if not os.path.exists(f)]
    if missing:
        print("missing model files:", missing, file=sys.stderr)
        return 2

    t0 = time.perf_counter()
    rec = vb.Recognizer(args.model, args.threads)
    load_s = time.perf_counter() - t0
    files = sorted(glob.glob(os.path.join(args.wav_dir, "cmd_[0-9][0-9].wav")))
    if not files:
        print("no cmd_*.wav under", args.wav_dir, file=sys.stderr)
        return 2

    print("model=%s threads=%d load=%.2fs files=%d noise_snr=%.0fdB"
          % (args.model, args.threads, load_s, len(files), args.snr))
    print("%-3s %-24s %-24s %-24s %s"
          % ("#", "clean", "8.46k ulaw", "8.46k noisy", "routes(clean/link)"))
    ok_clean = ok_link = ok_noisy = 0
    tot_clean = tot_link = 0.0

    for i, path in enumerate(files):
        rate, pcm = read_wav(path)
        t = time.perf_counter()
        clean = rec.transcribe(pcm, rate)
        tot_clean += time.perf_counter() - t

        link_pcm = simulate_link(pcm, rate)
        t = time.perf_counter()
        link = rec.transcribe(link_pcm, 8462)
        tot_link += time.perf_counter() - t

        noisy = ""
        if args.noisy:
            noisy_pcm = simulate_link(pcm, rate, noisy=True,
                                      snr_db=args.snr)
            noisy = rec.transcribe(noisy_pcm, 8462)

        rc, rl, rn = route(clean), route(link), route(noisy) if noisy else ""
        exp = EXPECTED.get(i, "")
        good_c = exp in rc
        good_l = exp in rl
        good_n = exp in rn if noisy else False
        ok_clean += int(good_c)
        ok_link += int(good_l)
        ok_noisy += int(good_n)
        print("%-3d %-24s %-24s %-24s %s/%s%s"
              % (i, clean[-22:], link[-22:],
                 noisy[-22:] if noisy else "-",
                 "OK" if good_c else "..",
                 "OK" if good_l else "..",
                 "  route=%s" % rl))

    n = len(files)
    print("summary: route %d/%d clean, %d/%d link%s; "
          "avg latency %.2fs clean, %.2fs link"
          % (ok_clean, n, ok_link, n,
             (" , %d/%d noisy" % (ok_noisy, n)) if args.noisy else "",
             tot_clean / n, tot_link / n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
