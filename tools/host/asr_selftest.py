#!/usr/bin/env python3
"""Minimal offline ASR self-test with sherpa-onnx Paraformer (Chinese)."""
import sys, wave, numpy as np, sherpa_onnx

MODEL = "/home/Oliweitz/voice_models/model.int8.onnx"
TOKENS = "/home/Oliweitz/voice_models/tokens.txt"

def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1, "need mono"
        sr = w.getframerate()
        pcm = w.readframes(w.getnframes())
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    return x, sr

def main():
    rec = sherpa_onnx.OfflineRecognizer.from_paraformer(
        paraformer=MODEL, tokens=TOKENS, num_threads=2,
        sample_rate=16000, feature_dim=80, decoding_method="greedy_search",
        debug=False,
    )
    print("recognizer ready")
    for path in sys.argv[1:]:
        x, sr = read_wav(path)
        s = rec.create_stream()
        s.accept_waveform(sample_rate=sr, waveform=x)
        rec.decode_stream(s)
        print("%-46s [%.2fs] -> %s" % (path.split('/')[-1], len(x)/sr, s.result.text))

main()
