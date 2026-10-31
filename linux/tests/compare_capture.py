#!/usr/bin/env python3
"""Compare a live capture from the FxSound PipeWire path with the offline DSP
reference. Prints alignment, gain, and residual vs. both the processed
reference and the unprocessed input (to prove processing really happened).
usage: compare_capture.py rec.wav tone.wav preset.fac dsp_process_raw [--bypass]

Run live and offline with the same, fresh XDG_CONFIG_HOME: the DSP restores
"last used" settings from its registry file at startup."""
import os, subprocess, sys, wave
import numpy as np

def read_wav(path):
    w = wave.open(path, "rb")
    n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, np.int16).astype(np.float32) / 32767
    else:   # pw-record writes 32-bit float WAVs (format 3), which the wave module reads as raw bytes
        x = np.frombuffer(raw, np.float32)
    return x.reshape(-1, ch), sr

def read_float_wav(path):
    # wave refuses WAVE_FORMAT_IEEE_FLOAT; parse the RIFF chunks directly
    d = open(path, "rb").read()
    i = 12; fmt = None
    while i < len(d) - 8:
        cid, size = d[i:i+4], int.from_bytes(d[i+4:i+8], "little")
        if cid == b"fmt ":
            fmt = (int.from_bytes(d[i+8:i+10], "little"), int.from_bytes(d[i+10:i+12], "little"),
                   int.from_bytes(d[i+12:i+16], "little"), int.from_bytes(d[i+22:i+24], "little"))
        if cid == b"data":
            tag, ch, sr, bits = fmt
            body = d[i+8:i+8+size]
            x = np.frombuffer(body[:len(body)//4*4], np.float32 if bits == 32 and tag in (3, 0xFFFE) else np.int32)
            return x.reshape(-1, ch).astype(np.float64), sr
        i += 8 + size + (size & 1)
    raise ValueError("no data chunk")

rec, sr = read_float_wav(sys.argv[1])
inp, sr2 = read_wav(sys.argv[2])
assert sr == sr2 == 48000, (sr, sr2)
import os
PAD = float(os.environ.get("COMPARE_PAD_SECONDS", "1"))   # silence the DSP saw before the tone
pad = np.zeros((int(sr * PAD), 2), np.float32)
src = np.concatenate([pad, inp]).astype(np.float32)
bypass = "--bypass" in sys.argv
ref_args = [sys.argv[4], sys.argv[3], "48000", "480"] + (["--bypass"] if bypass else [])
if os.environ.get("COMPARE_BANDS"):   # mirror the GUI's EQ band setting
    ref_args += ["--bands", os.environ["COMPARE_BANDS"]] + os.environ.get("COMPARE_EXTRA", "").split()
ref = np.frombuffer(subprocess.run(ref_args, input=src.tobytes(),
                                   capture_output=True).stdout, np.float32).reshape(-1, 2)[len(pad):].astype(np.float64)
inp = inp.astype(np.float64)

def rms(a): return float(np.sqrt(np.mean(a ** 2)))
def db(a, b): return 20 * np.log10(rms(a) / rms(b))

# Align the capture to the reference (cross-correlation via FFT on the left channel).
N = len(ref)
seg = rec[:, 0]
c = np.fft.irfft(np.fft.rfft(seg, len(seg) + N) * np.conj(np.fft.rfft(ref[:, 0], len(seg) + N)))
lag = int(np.argmax(c[:len(seg)]))
live = rec[lag:lag + N]
n = min(len(live), N); live, ref_, inp_ = live[:n], ref[:n], inp[:n]
s = slice(sr // 2, n - sr // 10)          # steady-state part (skip onset / tail)
print(f"capture: {len(rec)/sr:.2f}s, tone found at {lag/sr:.3f}s, compared {(s.stop-s.start)/sr:.2f}s")
print(f"level: input {20*np.log10(rms(inp_[s])):.2f} dBFS, live {20*np.log10(rms(live[s])):.2f} dBFS, "
      f"offline ref {20*np.log10(rms(ref_[s])):.2f} dBFS")
for name, other in (("offline DSP reference", ref_), ("unprocessed input", inp_)):
    corr = [float(np.corrcoef(live[s, ch], other[s, ch])[0, 1]) for ch in range(2)]
    resid = db(live[s] - other[s], other[s])
    print(f"vs {name:22s}: corr L={corr[0]:.5f} R={corr[1]:.5f}   residual {resid:+.1f} dB")
print(f"channel balance live L/R: {db(live[s,0:1], live[s,1:2]):+.2f} dB (input {db(inp_[s,0:1], inp_[s,1:2]):+.2f} dB)")
