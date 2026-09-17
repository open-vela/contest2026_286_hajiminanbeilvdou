#!/usr/bin/env python3
"""Check that recognition works when the audio is NOT at the model's rate.

The device captures at ~5.8 kHz and the model wants 16 kHz, so this feeds a
known-good 16 kHz recording to the same code path after crudely decimating it,
at the rate the device actually produces. If this returns text quickly, the
resampling hand-off to sherpa is working; if it pins every core and hangs, the
rational resampler has crept back into the path.

    python3 asr_rate_test.py [rate]
"""
import sys
import time
import wave

import numpy as np

sys.path.insert(0, "/home/Oliweitz")
from voice_bridge import Recognizer  # noqa: E402

RATE = int(sys.argv[1]) if len(sys.argv) > 1 else 5801
WAV = "/home/Oliweitz/mic_captures/utt_19.wav"

with wave.open(WAV, "rb") as w:
    src = w.getframerate()
    pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)

# Crude decimation is fine here: the point is the rate the recogniser is told
# about, not the fidelity of this particular conversion.
step = src / float(RATE)
idx = (np.arange(int(len(pcm) / step)) * step).astype(int)
dec = pcm[idx]

print("source %d Hz %d samples -> %.0f Hz %d samples (%.2f s)"
      % (src, len(pcm), RATE, len(dec), len(dec) / float(RATE)))

t0 = time.time()
text = Recognizer().transcribe(dec.tobytes(), RATE)
print("decode %.2fs -> %r" % (time.time() - t0, text))
