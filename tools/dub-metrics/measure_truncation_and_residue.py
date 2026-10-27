"""Third pass: a level that is not fooled by a shout in a mostly-silent file, the
bed-versus-output comparison where nobody is speaking, and what the three
truncated lines actually lost."""
import json
import os
import re
import wave
from pathlib import Path

import numpy as np

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK, DUB = JOB / "work", JOB / "dub_auto"
load = lambda n: json.load(open(WORK / n, encoding="utf-8"))


def read_wav(path):
    with wave.open(str(path)) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    d = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    return (d.reshape(-1, ch).mean(axis=1) if ch > 1 else d), rate


fit = {r["index"]: r for r in load("fit_auto.json")}
mix = {r["index"]: r for r in load("mix_auto.json")}
lines = {r["index"]: r for r in load("lines.json")}


def active_level(data, rate):
    """Mean energy of the frames that carry the line, not of the whole file.

    A 0.67 s file holding one shouted syllable is two thirds silence: a percentile
    over all frames reports the room, not the voice.
    """
    hop = int(rate * 0.02)
    n = len(data) // hop
    if n < 1:
        return None
    loud = 20 * np.log10(np.sqrt((data[: n * hop].reshape(n, hop) ** 2).mean(axis=1)) + 1e-9)
    keep = loud > max(-55.0, loud.max() - 25.0)
    if not keep.any():
        return None
    return float(10 * np.log10(np.mean(10 ** (loud[keep] / 10))))


levels = {}
for path in sorted(DUB.glob("zh*.wav")):
    i = int(re.sub(r"\D", "", path.stem) or -1)
    if i not in fit or i not in mix:
        continue
    data, rate = read_wav(path)
    lv = active_level(data, rate)
    if lv is None:
        continue
    levels[i] = lv + 20 * np.log10(mix[i]["gain"])

v = np.array(list(levels.values()))
print("=== delivered voice level, active frames only ===")
print(f"lines                        : {len(v)}")
print(f"p05 / median / p95           : {np.percentile(v,5):.1f} / {np.median(v):.1f} / {np.percentile(v,95):.1f} dBFS")
print(f"spread p95-p05               : {np.percentile(v,95)-np.percentile(v,5):.1f} dB")
med = float(np.median(v))
print(f"lines >3 / >6 dB off median  : {int((np.abs(v-med)>3).sum())} / {int((np.abs(v-med)>6).sum())}")
for i in sorted(levels, key=lambda i: -abs(levels[i] - med))[:5]:
    print(f"    #{i:<3} {levels[i]:6.1f} dBFS  gain x{mix[i]['gain']:.2f}  {fit[i]['seconds']:.2f}s  {fit[i]['text'][:24]}")

print()
print("=== what the three truncated lines lost ===")
for i in (17, 54, 55):
    f = fit[i]
    print(f"    #{i:<3} window {f['window']:.2f}s  spoke {f['seconds']:.2f}s  tempo x{f['tempo']:.2f}  "
          f"'{f['text']}'  -> cut by {max(0, f['seconds']-f['window']):.2f}s")

print()
print("=== voice ending before its cue ===")
short = []
for i, f in fit.items():
    cue = lines[i]["end"] - lines[i]["start"]
    spoken = min(f["seconds"], f["window"])
    short.append((cue - spoken, i))
short.sort(reverse=True)
print(f"lines where the subtitle outlasts the voice by >1.0 s: {sum(1 for d,_ in short if d>1.0)}")
for d, i in short[:5]:
    print(f"    #{i:<3} subtitle {lines[i]['end']-lines[i]['start']:.2f}s vs voice {fit[i]['seconds']:.2f}s  ({d:+.2f}s)")

print()
print("=== bed vs output where the old voice was but the dub is silent ===")
vox, rate = read_wav(JOB / "stems_out" / "vocals.wav")
bed, _ = read_wav(JOB / "stems_out" / "no_vocals.wav")
out, _ = read_wav(JOB / "dubbed_full_zh2.wav")
n = min(len(vox), len(bed), len(out))
hop = int(rate * 0.02)
frames = n // hop
env = lambda x: np.sqrt((x[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1) + 1e-12)
vv, bb, oo = env(vox[:n]), env(bed[:n]), env(out[:n])
silent = np.ones(frames, dtype=bool)
for f in fit.values():
    a, e = int(f["start"] / 0.02), int((f["start"] + f["seconds"]) / 0.02)
    silent[a:min(e, frames)] = False
pockets = (vv > 10 ** (-40 / 20)) & silent
ratio = 20 * np.log10(oo[pockets] / np.maximum(bb[pockets], 1e-12))
print(f"pockets: {pockets.sum()*0.02:.0f} s")
print(f"output minus bed             : median {np.median(ratio):+.2f} dB, p95 {np.percentile(ratio,95):+.2f} dB")
print(f"(0 dB means the old voice is gone; +6 dB would mean it is audible again)")
