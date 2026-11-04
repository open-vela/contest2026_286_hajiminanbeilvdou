#!/usr/bin/env python3
"""PC-side voice bridge for the Huangshan Pi AI Agent.

Captures the device microphone over the agent's WebSocket channel, runs
offline speech recognition on the PC, turns the transcript into a device
command, and hands that command back to the agent.

    device MEMS mic
      -> AUDCODEC ADC capture (sf32lb_audcodec.c)
      -> binary WebSocket frames @16 kHz / 16-bit mono
      -> this script: energy VAD -> sherpa-onnx offline ASR
         (FireRedASR2-AED by default when downloaded, Paraformer fallback)
      -> transcript -> wake phrase -> understanding step -> command
      -> {"type":"message","content":...} back to the device
      -> agent executes (cron tasks, telemetry, screen, ...)

Two ways to address the device:

  * the PTT button on the screen -- press to talk, the whole window is kept
    and recognised in one piece;
  * the wake phrase "你好openvela" -- say it, then the command. Both in one
    breath works too. A command spoken without it is ignored, which is what
    keeps room noise from firing commands at the agent.

Usage:
    python3 voice_bridge.py [--host H] [--port P] [--seconds N]
                            [--thresh N] [--no-vad] [--save-wav DIR]
                            [--no-wake]

The device side must be running the agent with CONFIG_SF32LB52_AUDCODEC and
the mic streaming support compiled in.
"""

from __future__ import annotations

import argparse
import asyncio
import difflib
import json
import math
import os
import re
import sys
import time
import traceback
import wave

import numpy as np
import sherpa_onnx
import websockets

# ---------------------------------------------------------------------------
# Audio / protocol constants -- must match the device side
# ---------------------------------------------------------------------------

# The device compands to G.711 mu-law at 8462 Hz, one byte per sample. A 60 s
# soak at this rate sustained 8.40 KB/s with zero ping loss; a 17.4 kHz /
# 4-bit ADPCM trial showed the practical link ceiling is ~8.0-8.4 KB/s, so
# this is the widest rate that stays reliable. The real rate arrives in the
# stream's audio_format frame; this is only the fallback.
DEVICE_RATE = 8462

# The device sends large frames (throughput), but endpointing wants small
# ones. Decouple the two here: whatever arrives is sliced back into fixed
# 100 ms pieces before the segmenter sees it, so tuning the device's frame
# size for the link never changes the VAD's resolution.
SUBFRAME_MS = 100

# Silence for this long means the link died rather than that nobody spoke.
# The device streams continuously (it does not gate capture on voice), so
# anything much longer than a few frame periods is a fault. Set well above
# the worst stall a busy agent produces, or a slow turn will look like a
# dead link.
STALL_S = 20.0

# Rate the recogniser expects. The device's formal rate is announced in an
# audio_format frame on stream start; this is the fallback.
MODEL_RATE = 16000

MODEL_DIR = os.path.expanduser("~/voice_models")
MODEL_ONNX = os.path.join(MODEL_DIR, "model.int8.onnx")
MODEL_TOKENS = os.path.join(MODEL_DIR, "tokens.txt")

# Available offline recognisers.  The first entry is the small model that
# shipped with the demo; the larger ones are drop-in upgrades for accuracy
# (FireRedASR2-AED and Qwen3-ASR are the strongest, at a latency cost).
ASR_MODELS = {
    "paraformer-small": {
        "kind": "paraformer", "dir": MODEL_DIR,
        "files": ("model.int8.onnx", "tokens.txt"),
    },
    "paraformer-large": {
        "kind": "paraformer", "dir": "~/voice_models/paraformer-large",
        "files": ("model.int8.onnx", "tokens.txt"),
    },
    "sensevoice": {
        "kind": "sense_voice", "dir": "~/voice_models/sensevoice-2025",
        "files": ("model.int8.onnx", "tokens.txt"),
    },
    "dolphin-small": {
        "kind": "dolphin", "dir": "~/voice_models/dolphin-small",
        "files": ("model.int8.onnx", "tokens.txt"),
    },
    "firered-aed": {
        "kind": "fire_red_aed", "dir": "~/voice_models/firered2-aed",
        "files": ("encoder.int8.onnx", "decoder.int8.onnx", "tokens.txt"),
    },
    "firered-ctc": {
        "kind": "fire_red_ctc", "dir": "~/voice_models/firered2-ctc",
        "files": ("model.int8.onnx", "tokens.txt"),
    },
    "qwen3-0.6b": {
        "kind": "qwen3", "dir": "~/voice_models/qwen3-0.6b",
        "files": ("conv_frontend.onnx", "encoder.int8.onnx",
                  "decoder.int8.onnx", "tokenizer.json"),
    },
}


def asr_model_paths(name: str) -> dict[str, str]:
    spec = ASR_MODELS[name]
    base = os.path.expanduser(spec["dir"])
    return {f: os.path.join(base, f) for f in spec["files"]}


def asr_model_files(name: str) -> tuple[str, ...]:
    return tuple(asr_model_paths(name).values())


def ulaw_decode(data: bytes) -> np.ndarray:
    """G.711 mu-law -> int16, matching mic_ulaw_encode() on the device."""
    u = (~np.frombuffer(data, dtype=np.uint8)).astype(np.int32) & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    sample = (((mantissa << 3) + 0x84) << exponent) - 0x84
    return np.where(sign != 0, -sample, sample).astype(np.int16)


IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

IMA_INDEX_TABLE = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)


class ImaAdpcmDecoder:
    """Stateful 4-bit IMA ADPCM decoder (low nibble first).

    The device carries an odd sample's nibble into the next WebSocket frame,
    so the decoder state must live across frames, not be recreated per frame.
    """

    def __init__(self):
        self.reset()

    def reset(self) -> None:
        self.prev = 0
        self.index = 0

    def decode(self, data: bytes) -> bytes:
        out = bytearray(len(data) * 4)   # 2 samples/byte * 2 bytes/sample
        pos = 0
        prev = self.prev
        index = self.index

        for byte in data:
            for nib in (byte & 0x0F, byte >> 4):
                step = IMA_STEP_TABLE[index]
                diff = step >> 3
                if nib & 4:
                    diff += step
                if nib & 2:
                    diff += step >> 1
                if nib & 1:
                    diff += step >> 2
                if nib & 8:
                    prev -= diff
                else:
                    prev += diff
                if prev < -32768:
                    prev = -32768
                elif prev > 32767:
                    prev = 32767

                index += IMA_INDEX_TABLE[nib]
                if index < 0:
                    index = 0
                elif index > 88:
                    index = 88

                sample = prev & 0xFFFF
                out[pos] = sample & 0xFF
                out[pos + 1] = sample >> 8
                pos += 2

        self.prev = prev
        self.index = index
        return bytes(out)


class DcBlocker:
    """One-pole high-pass (~80 Hz) with state kept across audio frames.

    The ADC has a DC offset and the board picks up desk/air-conditioner
    rumble. Neither carries speech, but both raise the RMS the VAD uses and
    can bias the recogniser's front end. Cutting below 80 Hz is inaudible
    for speech and removes the slow drift.
    """

    def __init__(self, rate: int, cutoff: float = 80.0):
        self.alpha = math.exp(-2.0 * math.pi * cutoff / float(max(rate, 1)))
        self.reset()

    def reset(self) -> None:
        self._x1 = 0.0
        self._y1 = 0.0

    def process(self, pcm: bytes) -> bytes:
        if len(pcm) < 2:
            return pcm
        x = np.frombuffer(pcm, dtype=np.int16).astype(np.float64)
        y = np.empty(len(x), dtype=np.float64)
        alpha = self.alpha
        x1 = self._x1
        y1 = self._y1
        for i in range(len(x)):
            xi = float(x[i])
            yi = alpha * (y1 + xi - x1)
            y[i] = yi
            x1 = xi
            y1 = yi
        self._x1 = x1
        self._y1 = y1
        return np.clip(np.round(y), -32768, 32767).astype(np.int16).tobytes()


def resample(x: np.ndarray, src: int, dst: int) -> np.ndarray:
    """Rational resample by zero-stuffing and low-pass filtering.

    A windowed-sinc filter is needed rather than plain interpolation: the
    images that plain interpolation leaves above the original Nyquist land
    right in the band the recogniser listens to.
    """
    if src == dst:
        return x

    from math import gcd

    g = gcd(src, dst)
    up, down = dst // g, src // g

    y = np.zeros(len(x) * up, dtype=np.float64)
    y[::up] = x.astype(np.float64)

    # Cutoff just below the original Nyquist, expressed in units of the
    # upsampled rate.
    ntaps = 8 * up + 1
    n = np.arange(ntaps) - (ntaps - 1) / 2.0
    h = np.sinc(n / up) * np.hamming(ntaps)
    h /= h.sum()

    y = np.convolve(y, h, mode="same")[::down]
    return np.clip(np.round(y), -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------------------
# Speech recognition
# ---------------------------------------------------------------------------

class Recognizer:
    """Thin wrapper over the selected sherpa-onnx offline ASR model.

    All models are fed at MODEL_RATE; the device rate is resampled by sherpa
    itself.  Larger models (FireRedASR2-AED, Qwen3-ASR) trade latency for
    accuracy but load through the same interface.
    """

    def __init__(self, model: str = "paraformer-small",
                 num_threads: int | None = None):
        if model not in ASR_MODELS:
            raise ValueError("unknown ASR model %r (choose from %s)"
                             % (model, ", ".join(sorted(ASR_MODELS))))
        self.model = model
        paths = asr_model_paths(model)
        kind = ASR_MODELS[model]["kind"]
        threads = num_threads or int(os.environ.get("VOICE_ASR_THREADS", "4"))
        print("[asr] model=%s threads=%d" % (model, threads), flush=True)
        common = dict(num_threads=threads, debug=False, provider="cpu")

        if kind == "paraformer":
            self._rec = sherpa_onnx.OfflineRecognizer.from_paraformer(
                paraformer=paths["model.int8.onnx"],
                tokens=paths["tokens.txt"],
                sample_rate=MODEL_RATE,
                feature_dim=80,
                decoding_method="greedy_search",
                **common,
            )
        elif kind == "sense_voice":
            self._rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
                model=paths["model.int8.onnx"],
                tokens=paths["tokens.txt"],
                sample_rate=MODEL_RATE,
                feature_dim=80,
                decoding_method="greedy_search",
                use_itn=True,
                language="zh",
                **common,
            )
        elif kind == "dolphin":
            self._rec = sherpa_onnx.OfflineRecognizer.from_dolphin_ctc(
                model=paths["model.int8.onnx"],
                tokens=paths["tokens.txt"],
                sample_rate=MODEL_RATE,
                feature_dim=80,
                decoding_method="greedy_search",
                **common,
            )
        elif kind == "fire_red_aed":
            self._rec = sherpa_onnx.OfflineRecognizer.from_fire_red_asr(
                encoder=paths["encoder.int8.onnx"],
                decoder=paths["decoder.int8.onnx"],
                tokens=paths["tokens.txt"],
                decoding_method="greedy_search",
                **common,
            )
        elif kind == "fire_red_ctc":
            self._rec = sherpa_onnx.OfflineRecognizer.from_fire_red_asr_ctc(
                model=paths["model.int8.onnx"],
                tokens=paths["tokens.txt"],
                decoding_method="greedy_search",
                **common,
            )
        elif kind == "qwen3":
            self._rec = sherpa_onnx.OfflineRecognizer.from_qwen3_asr(
                conv_frontend=paths["conv_frontend.onnx"],
                encoder=paths["encoder.int8.onnx"],
                decoder=paths["decoder.int8.onnx"],
                tokenizer=paths["tokenizer.json"],
                sample_rate=MODEL_RATE,
                feature_dim=128,
                decoding_method="greedy_search",
                **common,
            )
        else:
            raise ValueError("unsupported ASR model kind %r" % kind)

    def transcribe(self, pcm: bytes, src_rate: int = DEVICE_RATE) -> str:
        if len(pcm) < 2:
            return ""
        samples = np.frombuffer(pcm, dtype=np.int16)
        if samples.size < src_rate // 4:         # shorter than 250 ms
            return ""

        # The samples go in at the rate they were captured at and sherpa
        # resamples them itself. It has to: the resample() helper below is
        # rational, so it expands by dst/gcd(src, dst), and the device rate is
        # chosen for the serial link rather than for being commensurate with
        # 16 kHz. gcd(5801, 16000) is 1, so passing a few seconds of audio
        # through it allocates a ~5 GB zero-stuffed array and convolves it
        # with a 128k-tap filter -- it does not fail, it just pins every core
        # and hangs the bridge. Leaving it to sherpa avoids the whole problem.
        waveform = samples.astype(np.float32) / 32768.0
        stream = self._rec.create_stream()
        stream.accept_waveform(sample_rate=src_rate, waveform=waveform)
        self._rec.decode_stream(stream)
        return stream.result.text.strip()


# ---------------------------------------------------------------------------
# Energy based end-pointing
# ---------------------------------------------------------------------------

class Segmenter:
    """Split a continuous PCM stream into utterances.

    The noise floor is tracked while idle so the trigger threshold adapts to
    the room instead of being a magic constant that only works at one desk.
    """

    # Everything is expressed in milliseconds rather than frames: the device
    # decides its own chunk size, so a frame is not a fixed slice of time.
    # At 8 kHz the current 1600-sample chunk is 200 ms, not the 100 ms the
    # frame-counted version of this assumed.
    # The bridge slices incoming frames into fixed sub-frames before they get
    # here, so these are real times regardless of the device's frame size.
    PREROLL_MS = 300          # kept before the trigger point
    # 700 ms cut "你好openvela" away from the command whenever the speaker
    # paused for a breath. A raw 90 s capture showed the same sentence split
    # into "你好 OPEN VLOG" + "现在" + "几点了"; 1500 ms keeps the whole
    # command together at the cost of one extra second of response latency.
    TRAIL_SILENCE_MS = 1500   # silence that ends an utterance
    MIN_SPEECH_MS = 200       # ignore blips shorter than this
    MAX_UTTERANCE_MS = 15000  # hard stop
    CALIBRATION_MS = 600      # measured before the first decision

    # 2.5 was too strict: the on-board microphone sits close to the desk and
    # picks up a fair amount of broadband noise, so the gap between the floor
    # and a spoken phrase is only about 3x. 1.5 (down from 1.8) was validated
    # on the same raw capture: quiet syllables at ~3.3k RMS were being missed
    # with 1.8, and the recogniser then saw only "设备的内" instead of the
    # full "请帮我看一下设备的内存".
    HEADROOM = 1.5

    def __init__(self, rate: int = DEVICE_RATE, thresh: int = 300,
                 fixed_thresh: bool = False):
        self._rate = rate
        self._thresh = thresh
        self._fixed = fixed_thresh
        self._noise = float(thresh)
        self._calib: list[float] = []
        self._calib_ms = 0.0
        self._in_speech = False
        self._speech_ms = 0.0
        self._silence_ms = 0.0
        self._preroll: list[bytes] = []
        self._buf = bytearray()

    def set_rate(self, rate: int) -> None:
        self._rate = rate

    def _ms(self, frame: bytes) -> float:
        return len(frame) / 2.0 / max(self._rate, 1) * 1000.0

    @staticmethod
    def _rms(frame: bytes) -> float:
        x = np.frombuffer(frame, dtype=np.int16)
        if x.size == 0:
            return 0.0
        return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))

    def _threshold(self) -> float:
        if self._fixed:
            return float(self._thresh)
        return max(float(self._thresh), self._noise * self.HEADROOM)

    def _calibrating(self) -> bool:
        return not self._fixed and self._calib_ms < self.CALIBRATION_MS

    def push(self, frame: bytes) -> bytes | None:
        """Feed one captured chunk; return a complete utterance, or None."""
        rms = self._rms(frame)
        ms = self._ms(frame)

        # The room is measured before anything is allowed to trigger. Without
        # this the floor starts at the command-line value and takes seconds to
        # climb to the real ambient level, which is long enough to slice
        # steady background noise into a string of phantom utterances.
        if self._calibrating():
            self._calib.append(rms)
            self._calib_ms += ms
            self._noise = float(np.median(self._calib))
            if self._calib_ms >= self.CALIBRATION_MS:
                print("  [vad] ambient floor %.0f rms -> threshold %.0f"
                      % (self._noise, self._threshold()))
                self._preroll = []
            return None

        thr = self._threshold()

        if not self._in_speech:
            # Track the floor only while idle so speech never raises it.
            self._noise = 0.95 * self._noise + 0.05 * rms
            if rms > thr:
                self._in_speech = True
                self._speech_ms = ms
                self._silence_ms = 0.0
                self._buf = bytearray(b"".join(self._preroll))
                self._buf += frame
                self._preroll = []
                print("  [vad] speech start (rms=%d thr=%d)" % (rms, thr))
            else:
                self._preroll.append(frame)
                while (sum(len(f) for f in self._preroll)
                       > self.PREROLL_MS / 1000.0 * self._rate * 2):
                    self._preroll.pop(0)
                # Keep the preroll bounded even if the maths above is off.
                if len(self._preroll) > 8:
                    self._preroll.pop(0)
            return None

        self._buf += frame
        self._speech_ms += ms

        if rms > thr:
            self._silence_ms = 0.0
        else:
            self._silence_ms += ms

        done = (self._silence_ms >= self.TRAIL_SILENCE_MS
                or self._speech_ms >= self.MAX_UTTERANCE_MS)

        if not done:
            return None

        utt = bytes(self._buf)
        voiced_ms = self._speech_ms - self._silence_ms
        self._in_speech = False
        self._buf = bytearray()
        self._preroll = []
        self._silence_ms = 0.0

        if voiced_ms < self.MIN_SPEECH_MS:
            print("  [vad] too short, dropped (%.0f ms voiced)" % voiced_ms)
            return None
        print("  [vad] utterance: %.2f s voice in %.2f s" %
              (voiced_ms / 1000.0, self._speech_ms / 1000.0))
        return utt

    def flush(self) -> bytes | None:
        if self._in_speech and self._buf:
            utt = bytes(self._buf)
            self._in_speech = False
            self._buf = bytearray()
            return utt
        return None


# ---------------------------------------------------------------------------
# Understanding step
# ---------------------------------------------------------------------------

# Rules used when no LLM endpoint is configured. Order matters: the first
# match wins, so put the more specific intents first.
RULES: list[tuple[tuple[str, ...], str]] = [
    (("巡检", "巡查", "寻检", "巡捡", "设备状态", "检查设备",
      "检查一下设备", "体检"),
     "立即巡检设备，采集内存、任务数和存储数据"),
    # Cancel must come before the periodic rule: "取消定时任务" contains
    # "定时" and would otherwise be routed to "set another timer".
    (("停止", "取消", "别", "不用了", "取消掉", "关掉", "撤销"),
     "取消所有定时任务"),
    (("每隔", "每", "定时", "周期", "经常"),
     "请每2分钟巡检一次设备"),
    (("屏幕", "显示", "上屏", "写屏", "给我看"),
     "在屏幕上显示一句话：你好，我是随身AI管家"),
    (("内存", "存储", "空间", "还剩下", "运存", "记忆"),
     "看一下设备当前的内存和存储使用情况"),
    (("任务", "进程", "线程", "运行着什么", "跑着什么"),
     "看看设备上正在运行哪些任务"),
    (("时间", "几点", "多久", "报时"),
     "现在几点了"),
    (("介绍", "你是谁", "你能做什么", "会什么"),
     "你好，介绍一下你自己"),
    (("你好", "您好", "在吗", "嗨"),
     "你好，介绍一下你自己"),
]

_CN_DIGITS = {
    "零": 0, "一": 1, "二": 2, "两": 2, "三": 3, "四": 4,
    "五": 5, "六": 6, "七": 7, "八": 8, "九": 9,
}


def _parse_cn_number(text: str) -> int:
    """Parse the small Chinese/Arabic numbers used in 'every N minutes'."""
    text = text.strip()
    if not text:
        return 0
    if text.isdigit():
        return int(text)
    if "百" in text:
        left, _, right = text.partition("百")
        hundreds = _CN_DIGITS.get(left, 1) if left else 1
        return hundreds * 100 + (_parse_cn_number(right) if right else 0)
    if "十" in text:
        left, _, right = text.partition("十")
        tens = _CN_DIGITS.get(left, 1) if left else 1
        ones = _CN_DIGITS.get(right, 0) if right else 0
        return tens * 10 + ones
    return _CN_DIGITS.get(text, 0)


# ---------------------------------------------------------------------------
# Wake word
# ---------------------------------------------------------------------------

# The microphone already streams continuously, so the segmenter hears the
# whole room; the wake phrase is what decides which part of it the agent
# should act on. Exactly one phrase is accepted. Anything looser would let
# room noise fire commands -- the recogniser always returns its best guess,
# so an open microphone is a generator of plausible-looking nonsense.
WAKE_PHRASE = "你好openvela"

# Matching the whole phrase is not possible on this link. The microphone
# samples at 5801 Hz, so everything above ~2.9 kHz is gone before the
# recogniser sees it -- and the /v/ and /l/ that distinguish "vela" live up
# there. Measured over four attempts the tail came back as "open renline",
# "open rula", "ok v 乐" and "oppo via": never the brand, never twice the
# same. "你好" came back correct every time.
#
# So the rule pins the half that survives and only asks for evidence of the
# other: after the anchor, at least one ASCII letter. That is enough to keep
# an ordinary "你好，介绍一下你自己" -- or a burst of room noise -- from
# waking the device, while accepting every mangling seen in practice.
WAKE_ANCHORS = ("你好", "您好", "尼好")

# How long after the wake phrase a command is still accepted. Long enough to
# say the two halves in separate breaths, short enough that walking away does
# not leave the microphone live for whoever speaks next.
WAKE_ARM_S = 8.0


# Recurring ASR confusions on this band-limited link. Applied after
# normalisation, so they also make the wake tail contain a Latin word.
_ASR_FIXES = (
    ("寻检", "巡检"), ("巡捡", "巡检"), ("寻见", "巡检"), ("巡查", "巡检"),
    ("内层", "内存"), ("内村", "内存"), ("内纯", "内存"),
    ("定式", "定时"), ("定事", "定时"),
    ("任物", "任务"),
    ("平幕", "屏幕"), ("屏木", "屏幕"),
    ("记意", "记忆"), ("记玉", "记忆"), ("记一", "记忆"),
    # Wake-brand manglings measured on the 5.8 kHz stream; kept because the
    # recogniser still produces them at 8.46 kHz.
    ("openrenline", "openvela"), ("openrula", "openvela"),
    ("oppovia", "openvela"), ("okvela", "openvela"),
    ("openviila", "openvela"), ("openvilla", "openvela"),
    ("openvilller", "openvela"), ("openvillla", "openvela"),
    ("openvila", "openvela"), ("openvlog", "openvela"),
    ("openvlo", "openvela"),
    ("欧朋", "openvela"), ("欧本", "openvela"),
    ("欧维拉", "openvela"), ("欧文拉", "openvela"),
)


def normalize_speech(text: str) -> str:
    """Fold a transcript down to letters, digits and CJK characters.

    The recogniser will not spell a Latin brand the same way twice, and it
    sprinkles punctuation through Chinese, so "你好，openvela" and
    "你好 open vela" have to collapse to the same string before comparing.
    Known one-character slips are folded too; otherwise the intent rules
    never see the word they are looking for.
    """
    norm = "".join(ch for ch in text.lower() if ch.isalnum())
    for wrong, right in _ASR_FIXES:
        norm = norm.replace(wrong, right)
    return norm


WAKE_KEYS = tuple(normalize_speech(a) for a in WAKE_ANCHORS)


def split_wake(text: str) -> tuple[bool, str]:
    """Return (wake phrase heard, what was said after it).

    The tail matters: "你好openvela，现在几点了" is a wake word and a command
    in one breath, and the speaker should not have to say it twice.

    See WAKE_ANCHORS above for why the tail is only checked for the presence
    of a Latin letter rather than matched against the brand.
    """
    norm = normalize_speech(text)

    # The brand is distinctive enough that its presence anywhere is a wake.
    # FireRed frequently drops the leading "你" ("好 OPEN VILLA...") or
    # returns the brand alone; requiring a fixed prefix would miss those.
    pos = norm.find("openvela")
    if pos >= 0:
        return True, norm[pos + len("openvela"):]

    for key in WAKE_KEYS:
        if norm.startswith(key):
            tail = norm[len(key):]
            if any(ch.isascii() and ch.isalpha() for ch in tail):
                return True, tail
    return False, ""


def understand(text: str, llm_url: str | None = None) -> str:
    """Turn a transcript into an instruction for the device agent.

    The rule table is the offline path. When an LLM endpoint is configured it
    is preferred, because that is the point of the exercise -- but the demo
    has to work with no network and no API key.
    """
    if llm_url:
        try:
            return _understand_llm(text, llm_url)
        except Exception as exc:                     # noqa: BLE001
            print("  [llm] endpoint failed (%s), falling back to rules" % exc)

    text_n = normalize_speech(text)

    # "每五分钟" must keep the number; the generic 每/定时 rule below would
    # otherwise silently turn every interval into two minutes.
    m = re.search(r"每(?:隔)?([0-9零一二两三四五六七八九十百]+)(?:分钟|分)",
                  text_n)
    if m:
        minutes = _parse_cn_number(m.group(1))
        if 1 <= minutes <= 60:
            return "请每%d分钟巡检一次设备" % minutes

    for keys, command in RULES:
        if any(k in text_n for k in keys):
            return command

    # Short transcripts are the ones ASR mangles by one character ("巡捡",
    # "记意"). Accept a close match for those; longer free-form text still
    # falls through to the LLM or is dropped, so room noise cannot become a
    # command by accident.
    if len(text_n) <= 8:
        best_score = 0.0
        best_command = ""
        for keys, command in RULES:
            for key in keys:
                key_n = normalize_speech(key)
                if not key_n:
                    continue
                score = difflib.SequenceMatcher(None, text_n, key_n).ratio()
                if score > best_score:
                    best_score, best_command = score, command
        if best_score >= 0.72:
            print("  [rules] fuzzy %.2f -> %s" % (best_score, best_command))
            return best_command

    # Nothing matched, so this is not a command and the caller should drop it.
    #
    # This is a command-word interface, not open dictation, and the difference
    # matters more than it looks: the recogniser always produces its best
    # guess, so a burst of room noise comes back as fluent-looking Chinese
    # ("然后我觉得你", "中年卫地") with nothing to mark it as garbage. Handing
    # those to the device turns it into a generator of nonsense commands that
    # fire every few seconds, and the operator has no way to tell which lines
    # were actually spoken. Requiring a match costs the ability to say
    # something the table does not know about, which is a fair trade for a
    # demo that has to be predictable.
    return ""


def _understand_llm(text: str, llm_url: str) -> str:
    import urllib.request

    prompt = (
        "你是一个嵌入式设备语音助手。把用户的语音指令改写成一句清晰的中文指令，"
        "只输出改写后的指令本身，不要解释。\n\n用户说：%s" % text
    )
    body = json.dumps({
        "model": os.environ.get("VOICE_LLM_MODEL", "deepseek-chat"),
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0,
    }).encode()

    req = urllib.request.Request(
        llm_url, data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer %s" % os.environ.get("VOICE_LLM_KEY", ""),
        })
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"].strip()


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------

class Bridge:
    def __init__(self, args):
        self.args = args
        self.asr = Recognizer(args.asr_model, args.asr_threads)
        self.seg = Segmenter(rate=DEVICE_RATE, thresh=args.thresh,
                             fixed_thresh=args.no_vad)
        print("[vad] trail=%dms headroom=%.1f"
              % (self.seg.TRAIL_SILENCE_MS, self.seg.HEADROOM), flush=True)
        self.saved = 0
        self.rate = None          # set from the device's audio_format frame
        self.codec = "ulaw"       # updated by audio_format; ima_adpcm is 17.4 kHz
        self.adpcm = ImaAdpcmDecoder()
        self.dc = DcBlocker(DEVICE_RATE)
        self._carry = b""         # partial sub-frame left over between frames

        # Push-to-talk, driven by the button on the watch. While it is on,
        # incoming audio is accumulated here instead of going to the
        # segmenter, and the whole window is recognised in one go.
        self._ptt_on = False
        self._ptt_buf = bytearray()
        self._ptt_min_s = 0.3

        # Set when the wake phrase is heard, and cleared when the command it
        # armed arrives or the window expires.
        self._wake_until = 0.0

        # Filled in by run(), so the control-frame handler can start a
        # recognition the same way the audio path does.
        self._ws = None
        self._pending: list = []

        # Microphone is dropped while the agent is answering, so the LLM
        # request is not sharing the wire with the audio uplink.
        self._mic_paused = False
        self._resume_mic = False

    def decode_audio(self, data: bytes) -> bytes:
        if self.codec == "ima_adpcm":
            return self.adpcm.decode(data)
        return ulaw_decode(data).tobytes()

    def _feed(self, ws, pcm: bytes, pending: list) -> None:
        """Slice one received frame into fixed sub-frames for the segmenter."""
        pcm = self.dc.process(pcm)

        if self._ptt_on:
            # Push-to-talk owns the microphone while it is pressed. Running
            # the segmenter as well would cut the sentence at the first pause
            # and send half a command, so the whole window is kept and handed
            # over in one piece when the button is released.
            self._ptt_buf += pcm
            return

        self._carry += pcm
        step = int((self.rate or DEVICE_RATE) * SUBFRAME_MS / 1000) * 2

        while len(self._carry) >= step:
            sub = self._carry[:step]
            self._carry = self._carry[step:]
            self._log_audio(sub)
            utt = self.seg.push(sub)
            if utt:
                # Recognise off the receive path so frames keep draining
                # while the model runs.
                pending.append(self._spawn(self.on_utterance(ws, utt)))

    @staticmethod
    def _spawn(coro) -> "asyncio.Task":
        """Start the recognition task and make sure its failures are visible.

        A task created with create_task() that raises has nowhere to report:
        the exception sits on the task until something awaits it, and the only
        await here happens when the stream ends. Recognition failing -- a
        missing model, an unreadable waveform -- therefore looks exactly like
        nothing having been said, which is a very expensive thing to debug.
        """
        task = asyncio.create_task(coro)
        print("  [asr] task scheduled", flush=True)

        def _done(t: "asyncio.Task") -> None:
            if t.cancelled():
                print("  [asr] task was cancelled", flush=True)
                return
            exc = t.exception()
            if exc is not None:
                print("  [asr] FAILED: %r" % (exc,), flush=True)
                traceback.print_exception(type(exc), exc, exc.__traceback__)

        task.add_done_callback(_done)
        return task


    def save_wav(self, pcm: bytes, tag: str) -> None:
        if not self.args.save_wav:
            return
        os.makedirs(self.args.save_wav, exist_ok=True)
        # Written at the capture rate, untouched. Going through resample()
        # here would hit the same blow-up transcribe() avoids, and the raw
        # rate is what is actually worth listening to when a capture is bad.
        rate = self.rate or DEVICE_RATE
        samples = np.frombuffer(pcm, dtype=np.int16)
        self.saved += 1
        path = os.path.join(self.args.save_wav, "%s_%02d.wav" % (tag, self.saved))
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(samples.tobytes())
        print("  [wav] wrote %s (%d Hz, %d samples)"
              % (path, rate, samples.size))

    async def notify_wake(self, ws) -> None:
        """Put something on the screen when the wake phrase lands.

        Arming is invisible on its own: the device's recorder already streams,
        so nothing changes until a command arrives several seconds later. That
        leaves both the speaker and anyone watching unable to tell a successful
        wake from a missed one.

        The obvious cue -- lighting up the on-screen "录音中" indicator -- is
        not available: it is driven by the same ptt frames the button sends,
        and the device broadcasts those to every client, so the bridge would
        receive its own frame and believe the physical button had been pressed.
        A ui_notice frame reaches the UI queue without entering the agent.
        """
        try:
            await ws.send(json.dumps({"type": "ui_notice",
                                      "content": "（已唤醒，请说指令）"}))
            print("  [wake] 已在屏幕上提示")
        except Exception as exc:                     # noqa: BLE001
            print("  [wake] could not notify the device: %s" % exc)

    async def notify_dropped(self, ws) -> None:
        """Tell the device that a PTT utterance produced no command.

        Releasing the PTT button latches is_processing on the device, and the
        only thing that clears it is a reply landing on the UI queue. The same
        latch also blocks the button (ptt_btn_event_cb returns early while it
        is set), so an utterance the recogniser could not turn into a command
        used to leave the screen on "识别中..." with push-to-talk dead until
        the board was reset.

        A ui_notice unlatches it without starting an agent turn, and saying
        so beats freezing with no explanation.
        """
        try:
            await ws.send(json.dumps({"type": "ui_notice",
                                      "content": "（没听清，请再说一次）"}))
            print("  [cmd] notified the device that nothing was understood")
        except Exception as exc:                     # noqa: BLE001
            print("  [cmd] could not notify the device: %s" % exc)

    async def on_utterance(self, ws, pcm: bytes, via_ptt: bool = False) -> None:
        print("  [asr] start (%d bytes)" % len(pcm), flush=True)
        self.save_wav(pcm, "utt")

        t0 = time.time()
        text = self.asr.transcribe(pcm, self.rate or DEVICE_RATE)
        dt = time.time() - t0

        if not text:
            print("  [asr] (nothing recognised, %.2fs)" % dt)
            if via_ptt:
                await self.notify_dropped(ws)
            return
        print("  [asr] %.2fs  ->  %s" % (dt, text))

        # The wake check runs before understand(), because the rule table has
        # a "你好" entry: without this ordering the wake phrase would match it
        # and be dispatched as a command to introduce itself.
        #
        # The PTT button is already an explicit "this part was meant for you",
        # so it bypasses the gate rather than demanding the phrase as well.
        if not via_ptt:
            heard, rest = split_wake(text)
            if heard and rest:
                # Usually the tail is the brand name as the link mangled it
                # ("你好 oppo via" is the wake phrase on its own), not a
                # command. Try it as one so a genuine same-breath command
                # still works, but if it is not, arm and wait rather than
                # dropping the turn -- the speaker has already said the wake
                # word and should not have to say it again.
                command = understand(rest, self.args.llm_url)
                if command:
                    print("  [wake] 唤醒词与指令同句 -> %s" % rest)
                else:
                    self._wake_until = time.time() + WAKE_ARM_S
                    print("  [wake] 已唤醒 -- 请在 %.0f 秒内说出指令" % WAKE_ARM_S)
                    await self.notify_wake(ws)
                    return
            elif heard:
                self._wake_until = time.time() + WAKE_ARM_S
                print("  [wake] 已唤醒 -- 请在 %.0f 秒内说出指令" % WAKE_ARM_S)
                await self.notify_wake(ws)
                return
            elif self.args.no_wake:
                command = understand(text, self.args.llm_url)
            elif time.time() < self._wake_until:
                self._wake_until = 0.0
                print("  [wake] 已唤醒，接收指令")
                command = understand(text, self.args.llm_url)
            else:
                # Printed rather than silently dropped: during tuning this is
                # how you find out what the recogniser made of a phrase the
                # matcher did not accept.
                print("  [wake] 未唤醒，忽略：%s" % text)
                return
        else:
            command = understand(text, self.args.llm_url)

        if not command:
            print("  [cmd] (not a known command, ignored)")
            if via_ptt:
                await self.notify_dropped(ws)
            return
        print("  [cmd] -> %s" % command)

        # Drop the microphone while the agent works on this.
        #
        # The uplink carries about 6 KB/s and the stream uses most of it, so
        # an LLM request sharing the wire takes far longer than the request
        # itself needs -- measured at 83 s against a hosted endpoint, which
        # is what made it look like a network failure. Nothing is listening
        # during the turn anyway. It is resumed when the answer comes back
        # (see the run loop).
        await ws.send(json.dumps({"type": "mic_stop"}))
        self._mic_paused = True

        await ws.send(json.dumps({"type": "message", "content": command}))

    async def run(self) -> None:
        url = "ws://%s:%d" % (self.args.host, self.args.port)
        delay = 2.0

        while True:
            try:
                await self._session(url)
                return
            except asyncio.CancelledError:
                raise
            except Exception as exc:                 # noqa: BLE001
                print("[bridge] session ended (%r); reconnecting in %.0fs"
                      % (exc, delay), flush=True)
                await asyncio.sleep(delay)
                delay = min(delay * 2.0, 15.0)

    async def _session(self, url: str) -> None:
        print("[bridge] connecting to %s" % url)

        # No protocol keepalive. The far end runs the audio stream, its
        # replies and the keepalive pongs through one mutex, so a busy
        # stretch can hold a pong past any timeout worth setting -- and the
        # library's response to that is to drop a session that is working
        # fine. The audio is the real proof of life, and the stall check
        # below is stricter than a ping ever was.
        async with websockets.connect(url, max_size=None,
                                      ping_interval=None) as ws:
            print("[bridge] connected; starting device capture")
            await ws.send(json.dumps({"type": "mic_start"}))

            # Clear any stale "识别中..." latch left on the device.
            #
            # Releasing the PTT button sets is_processing, and the only thing
            # that clears it is a reply landing on the UI queue -- the same
            # flag also blocks the button (lvgl_ui_channel.c ptt_btn_event_cb
            # returns early while it is set). So a button press that happened
            # while this bridge was down leaves push-to-talk dead until the
            # board is reset, and nothing on this side can tell that it did.
            #
            # A ui_notice unlatches it without entering the agent loop, so ask
            # for one on connect. That is what makes the chain recover on its
            # own after a reconnect instead of needing someone to power-cycle
            # the board.
            await ws.send(json.dumps({"type": "ui_notice",
                                      "content": "（语音链路已连接）"}))

            # Fresh segmenter state: half a sentence from the previous
            # session must not be glued onto the first one of this one.
            self.seg = Segmenter(rate=self.rate or DEVICE_RATE,
                                 thresh=self.args.thresh,
                                 fixed_thresh=self.args.no_vad)
            self._carry = b""
            self._ptt_on = False
            self._ptt_buf = bytearray()

            deadline = (time.time() + self.args.seconds) if self.args.seconds else None
            pending: list[asyncio.Task] = []
            self._ws = ws
            self._pending = pending
            pcm_total = 0
            last_data = time.time()

            # A quiet room produces no output at all, which makes a working
            # bridge indistinguishable from a dead one. Emit a heartbeat so
            # "is it listening?" has an answer without speaking first.
            hb_last = time.time()
            hb_bytes = 0

            try:
                while True:
                    if deadline and time.time() > deadline:
                        print("[bridge] time limit reached")
                        break

                    timeout = None
                    if deadline:
                        timeout = max(0.1, deadline - time.time())

                    try:
                        msg = await asyncio.wait_for(ws.recv(), timeout=timeout)
                    except asyncio.TimeoutError:
                        break

                    now = time.time()

                    # The device streams continuously, so silence for this
                    # long means the link is gone, not that nothing was said.
                    # Raising hands the problem to run(), which reconnects --
                    # better than sitting here looking healthy forever.
                    if self.args.seconds == 0 and now - last_data > STALL_S:
                        raise TimeoutError(
                            "no audio for %.0fs" % (now - last_data))

                    if now - hb_last >= 10.0:
                        rate = (pcm_total - hb_bytes) / (now - hb_last)
                        state = ("CAPTURING" if self.rate else "waiting")
                        print("  [alive] %s  %.1f KB/s  %d bytes total"
                              % (state, rate / 1024.0, pcm_total))
                        hb_last = now
                        hb_bytes = pcm_total

                    if isinstance(msg, (bytes, bytearray)):
                        pcm_total += len(msg)
                        last_data = now
                        # Decode according to audio_format; both codecs end up
                        # as 16-bit mono PCM before levels or timing are used.
                        self._feed(ws, self.decode_audio(bytes(msg)), pending)
                    else:
                        self._on_text(msg)

                        # The answer arrived, so the microphone can come back.
                        if self._resume_mic:
                            self._resume_mic = False
                            self._mic_paused = False
                            await ws.send(json.dumps({"type": "mic_start"}))
                            print("  [mic] resumed")

                    pending = [t for t in pending if not t.done()]
            finally:
                utt = self.seg.flush()
                if utt:
                    await self.on_utterance(ws, utt)
                if pending:
                    await asyncio.gather(*pending, return_exceptions=True)
                try:
                    await ws.send(json.dumps({"type": "mic_stop"}))
                except Exception:                     # noqa: BLE001
                    pass
                print("[bridge] received %d bytes of audio (%.1f s at %d Hz)"
                      % (pcm_total, pcm_total / float(self.rate or DEVICE_RATE),
                         self.rate or DEVICE_RATE))

    _last_level = 0.0

    def _log_audio(self, frame: bytes) -> None:
        if not self.args.verbose:
            return
        now = time.time()
        if now - self._last_level < 0.5:
            return
        self._last_level = now
        rms = Segmenter._rms(frame)
        bar = "#" * min(60, int(rms / 100))
        print("  [lvl] %5d %s" % (rms, bar))

    def _on_text(self, msg: str) -> None:
        try:
            data = json.loads(msg)
        except ValueError:
            print("  [dev] %s" % msg)
            return

        kind = data.get("type", "?")
        content = data.get("content", "")

        if kind == "status":
            # Intermediate progress ("working..."). The agent has not
            # answered yet, so the uplink must stay paused.
            print("  [dev] status: %s" % content)
            return

        if kind == "health":
            # Device-side health transition or periodic report. It is not
            # speech and must not enter the recognition path; printing it
            # is enough for the watchdog log and for whoever is watching.
            issue = data.get("issue", "none")
            detail = data.get("detail", "")
            line = "  [health] %s" % data.get("status", "?")
            if issue and issue != "none":
                line += " -- %s" % issue
                if detail:
                    line += " (%s)" % detail
            print(line)
            return

        if kind == "response" and self._mic_paused:
            # The agent has answered, so the wire is free again. Actually
            # sending is the caller's job: this runs synchronously from the
            # receive loop and cannot await.
            self._resume_mic = True

        if kind == "audio_format":
            self.rate = int(data.get("rate", DEVICE_RATE))
            codec = data.get("codec")
            if codec:
                self.codec = codec
            if self.codec == "ima_adpcm":
                self.adpcm.reset()
            print("  [dev] stream format: %s %d Hz %d ch"
                  % (codec or "?", self.rate,
                     int(data.get("channels", 1))))
            if codec not in (None, "ulaw", "ima_adpcm"):
                print("  [dev] WARNING: unexpected codec; decoding as ulaw anyway")
                self.codec = "ulaw"
            self.seg.set_rate(self.rate)
            self.dc = DcBlocker(self.rate)
            return

        if kind == "ptt":
            # The watch's push-to-talk button. Audio flows the whole time
            # regardless; the button only says which part of it was meant as
            # a command, which is what makes a noisy room workable.
            state = data.get("state", "")
            if state == "start":
                self._ptt_on = True
                self._ptt_buf = bytearray()
                print("  [ptt] listening -- press the button again to finish")
            elif state == "stop" and self._ptt_on:
                self._ptt_on = False
                pcm = bytes(self._ptt_buf)
                self._ptt_buf = bytearray()
                secs = len(pcm) / 2.0 / float(self.rate or DEVICE_RATE)
                if secs < self._ptt_min_s:
                    # Dropping this silently is what wedged the button: the
                    # device latched is_processing when it sent "stop", and
                    # only a reply clears it. A double-tap (two clicks with
                    # no speech between them) is the easiest mistake to make
                    # with a toggle button, so it must answer too.
                    print("  [ptt] only %.2fs captured, ignored" % secs)
                    self._pending.append(
                        self._spawn(self.notify_dropped(self._ws)))
                else:
                    print("  [ptt] captured %.2fs, recognising" % secs)
                    self._pending.append(
                        self._spawn(self.on_utterance(self._ws, pcm,
                                                      via_ptt=True)))
            return

        print("  [dev:%s] %s" % (kind, content))


# ---------------------------------------------------------------------------

def default_asr_model() -> str:
    """Prefer the high-accuracy model when it has been downloaded."""
    env = os.environ.get("VOICE_ASR_MODEL")
    if env:
        return env
    if all(os.path.exists(f) for f in asr_model_files("firered-aed")):
        return "firered-aed"
    return "paraformer-small"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.223.2",
                   help="device address (the PPP peer; see demo_up.sh)")
    p.add_argument("--port", type=int, default=28789)
    p.add_argument("--seconds", type=float, default=0,
                   help="stop after N seconds (0 = run until Ctrl-C)")
    p.add_argument("--thresh", type=int, default=300,
                   help="VAD RMS floor")
    p.add_argument("--no-vad", action="store_true",
                   help="use a fixed threshold instead of adapting to the room")
    p.add_argument("--save-wav", default=None,
                   help="directory to dump captured utterances into")
    p.add_argument("--no-wake", action="store_true",
                   help="accept any recognised command without requiring the "
                        "wake phrase '%s' first" % WAKE_PHRASE)
    p.add_argument("--llm-url", default=os.environ.get("VOICE_LLM_URL"),
                   help="optional OpenAI-compatible endpoint for the "
                        "understanding step")
    p.add_argument("--verbose", action="store_true",
                   help="print a live level meter")
    p.add_argument("--asr-model", default=default_asr_model(),
                   choices=sorted(ASR_MODELS),
                   help="offline recogniser to load")
    p.add_argument("--asr-threads", type=int,
                   default=int(os.environ.get("VOICE_ASR_THREADS", "4")),
                   help="ONNX Runtime CPU threads for the recogniser")
    args = p.parse_args()

    for path in asr_model_files(args.asr_model):
        if not os.path.exists(path):
            print("missing model file: %s" % path, file=sys.stderr)
            return 2

    try:
        asyncio.run(Bridge(args).run())
    except KeyboardInterrupt:
        print("\n[bridge] interrupted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
