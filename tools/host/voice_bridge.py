#!/usr/bin/env python3
"""PC-side voice bridge for the Huangshan Pi AI Agent.

Captures the device microphone over the agent's WebSocket channel, runs
offline speech recognition on the PC, turns the transcript into a device
command, and hands that command back to the agent.

    device MEMS mic
      -> AUDCODEC ADC capture (sf32lb_audcodec.c)
      -> binary WebSocket frames @16 kHz / 16-bit mono
      -> this script: energy VAD -> sherpa-onnx Paraformer (zh, offline)
      -> transcript -> understanding step -> command
      -> {"type":"message","content":...} back to the device
      -> agent executes (cron tasks, telemetry, screen, ...)

Usage:
    python3 voice_bridge.py [--host H] [--port P] [--seconds N]
                            [--thresh N] [--no-vad] [--save-wav DIR]

The device side must be running the agent with CONFIG_SF32LB52_AUDCODEC and
the mic streaming support compiled in.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
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

# The device compands to G.711 mu-law at 5801 Hz, one byte per sample. The
# rate is what the PPP link can carry (~6 KB/s of payload), not what the
# recogniser would prefer; going wider does not degrade gracefully -- the
# capture path drops samples and the gaps alias into something no recogniser
# can read. The real rate arrives in the stream's audio_format frame; this is
# only the fallback.
DEVICE_RATE = 5801

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


def ulaw_decode(data: bytes) -> np.ndarray:
    """G.711 mu-law -> int16, matching mic_ulaw_encode() on the device."""
    u = (~np.frombuffer(data, dtype=np.uint8)).astype(np.int32) & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    sample = (((mantissa << 3) + 0x84) << exponent) - 0x84
    return np.where(sign != 0, -sample, sample).astype(np.int16)


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
    """Thin wrapper over the sherpa-onnx Paraformer Chinese model."""

    def __init__(self, onnx: str = MODEL_ONNX, tokens: str = MODEL_TOKENS):
        self._rec = sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=onnx,
            tokens=tokens,
            num_threads=2,
            sample_rate=MODEL_RATE,
            feature_dim=80,
            decoding_method="greedy_search",
            debug=False,
        )

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
    TRAIL_SILENCE_MS = 700    # silence that ends an utterance
    MIN_SPEECH_MS = 200       # ignore blips shorter than this
    MAX_UTTERANCE_MS = 15000  # hard stop
    CALIBRATION_MS = 600      # measured before the first decision

    # 2.5 was too strict: the on-board microphone sits close to the desk and
    # picks up a fair amount of broadband noise, so the gap between the floor
    # and a spoken phrase is only about 3x. With the trigger set that high,
    # inter-syllable dips fell back under it and utterances were cut off after
    # a few hundred milliseconds.
    HEADROOM = 1.8

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
    (("巡检", "检查一下", "体检"),
     "立即巡检设备，采集内存、任务数和存储数据"),
    (("每隔", "每", "定时", "周期", "经常"),
     "请每2分钟巡检一次设备"),
    (("停止", "取消", "别", "不用了", "取消掉"),
     "取消所有定时任务"),
    (("屏幕", "显示", "给我看"),
     "在屏幕上显示一句话：你好，我是随身AI管家"),
    (("内存", "存储", "空间", "还剩下"),
     "看一下设备当前的内存和存储使用情况"),
    (("任务", "进程", "运行着什么", "跑着什么"),
     "看看设备上正在运行哪些任务"),
    (("时间", "几点", "多久"),
     "现在几点了"),
    (("你好", "在吗", "嗨"),
     "你好，介绍一下你自己"),
]


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

    for keys, command in RULES:
        if any(k in text for k in keys):
            return command

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
        self.asr = Recognizer()
        self.seg = Segmenter(rate=DEVICE_RATE, thresh=args.thresh,
                             fixed_thresh=args.no_vad)
        self.saved = 0
        self.rate = None          # set from the device's audio_format frame
        self._carry = b""         # partial sub-frame left over between frames

        # Push-to-talk, driven by the button on the watch. While it is on,
        # incoming audio is accumulated here instead of going to the
        # segmenter, and the whole window is recognised in one go.
        self._ptt_on = False
        self._ptt_buf = bytearray()
        self._ptt_min_s = 0.3

        # Filled in by run(), so the control-frame handler can start a
        # recognition the same way the audio path does.
        self._ws = None
        self._pending: list = []

        # Microphone is dropped while the agent is answering, so the LLM
        # request is not sharing the wire with the audio uplink.
        self._mic_paused = False
        self._resume_mic = False

    def _feed(self, ws, pcm: bytes, pending: list) -> None:
        """Slice one received frame into fixed sub-frames for the segmenter."""
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

    async def on_utterance(self, ws, pcm: bytes) -> None:
        print("  [asr] start (%d bytes)" % len(pcm), flush=True)
        self.save_wav(pcm, "utt")

        t0 = time.time()
        text = self.asr.transcribe(pcm, self.rate or DEVICE_RATE)
        dt = time.time() - t0

        if not text:
            print("  [asr] (nothing recognised, %.2fs)" % dt)
            return
        print("  [asr] %.2fs  ->  %s" % (dt, text))

        command = understand(text, self.args.llm_url)
        if not command:
            print("  [cmd] (not a known command, ignored)")
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
                        # One mu-law byte per sample on the wire; expand to
                        # 16-bit before anything looks at levels or timing.
                        self._feed(ws, ulaw_decode(bytes(msg)).tobytes(),
                                   pending)
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

        if kind == "response" and self._mic_paused:
            # The agent has answered, so the wire is free again. Actually
            # sending is the caller's job: this runs synchronously from the
            # receive loop and cannot await.
            self._resume_mic = True

        if kind == "audio_format":
            self.rate = int(data.get("rate", DEVICE_RATE))
            print("  [dev] stream format: %s %d Hz %d ch"
                  % (data.get("codec", "?"), self.rate,
                     int(data.get("channels", 1))))
            if data.get("codec") not in (None, "ulaw"):
                print("  [dev] WARNING: unexpected codec; decoding as ulaw anyway")
            self.seg.set_rate(self.rate)
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
                    print("  [ptt] only %.2fs captured, ignored" % secs)
                else:
                    print("  [ptt] captured %.2fs, recognising" % secs)
                    self._pending.append(
                        self._spawn(self.on_utterance(self._ws, pcm)))
            return

        print("  [dev:%s] %s" % (kind, content))


# ---------------------------------------------------------------------------

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
    p.add_argument("--llm-url", default=os.environ.get("VOICE_LLM_URL"),
                   help="optional OpenAI-compatible endpoint for the "
                        "understanding step")
    p.add_argument("--verbose", action="store_true",
                   help="print a live level meter")
    args = p.parse_args()

    for path in (MODEL_ONNX, MODEL_TOKENS):
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
