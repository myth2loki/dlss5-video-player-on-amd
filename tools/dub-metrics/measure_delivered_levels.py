"""Second pass: the delivered levels (after the mix's gain), the overlap, and
how much of the original voice survived the separation."""
import json
import os
import re
import unicodedata
from pathlib import Path

import numpy as np

import wavio

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK, DUB = JOB / "work", JOB / "dub_auto"
load = lambda n: json.load(open(WORK / n, encoding="utf-8"))


def width(t):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in t)


read_wav = wavio.read_wav

lines = {r["index"]: r for r in load("lines.json")}
fit = {r["index"]: r for r in load("fit_auto.json")}
mix = {r["index"]: r for r in load("mix_auto.json")}

# ------------------------------------------- subtitles as they are spoken now
print("=== subtitles (text = what the mixer was given) ===")
rows = []
for i, f in sorted(fit.items()):
    text = (f.get("text") or "").strip()
    dur = lines[i]["end"] - lines[i]["start"]
    rows.append((i, text, dur, len(text), width(text)))
sp = np.array([r[3] / r[2] for r in rows if r[3] and r[2] > 0])
print(f"reading speed median/p95/max : {np.median(sp):.1f} / {np.percentile(sp,95):.1f} / {sp.max():.1f} c/s")
print(f"cues over 9 / 12 chars/s     : {int((sp>9).sum())} / {int((sp>12).sum())}")
w = np.array([r[4] for r in rows])
print(f"width median/max             : {np.median(w):.0f} / {w.max():.0f}   over 32: {int((w>32).sum())}")
for r in sorted(rows, key=lambda r: -r[4])[:3]:
    print(f"    #{r[0]:<3} {r[4]:3d}u {r[2]:.2f}s  {r[1]}")

gaps = []
order = sorted(fit)
for a, b in zip(order, order[1:]):
    gaps.append((lines[b]["start"] - lines[a]["end"], a, b))
overl = [g for g in gaps if g[0] < 0]
print(f"overlapping cues             : {len(overl)}  {[(f'#{a}->{b}', round(g,2)) for g,a,b in overl]}")
print(f"gaps under 0.08 s            : {sum(1 for g,_,_ in gaps if 0 <= g < 0.08)}")

# --------------------------------------- levels as delivered, not as synthesized
print()
print("=== delivered voice levels (synthesis + mix gain) ===")
delivered, notes = {}, {}
# Only the clips as synthesised: the mixer leaves zhNN_p.wav behind when it had to
# rush a line, and a glob that picks those up measures the same line twice.
for path in sorted(DUB.glob("zh[0-9][0-9].wav")):
    i = int(re.sub(r"\D", "", path.stem) or -1)
    if i not in fit or i not in mix:
        continue
    data, rate = read_wav(path)
    hop = int(rate * 0.02)
    n = len(data) // hop
    if n < 1:
        continue
    loud = 20 * np.log10(np.sqrt((data[: n * hop].reshape(n, hop) ** 2).mean(axis=1)) + 1e-9)
    speech = float(np.percentile(loud, 75))
    gain = mix[i]["gain"]
    delivered[i] = speech + 20 * np.log10(gain)
    notes[i] = (speech, gain, len(data) / rate, np.abs(data).max() * gain)

lv = np.array(list(delivered.values()))
print(f"lines                        : {len(lv)}")
print(f"delivered level p05/med/p95  : {np.percentile(lv,5):.1f} / {np.median(lv):.1f} / {np.percentile(lv,95):.1f} dBFS")
print(f"spread p95-p05               : {np.percentile(lv,95)-np.percentile(lv,5):.1f} dB")
print(f"lines >3 dB off median       : {int((np.abs(lv-np.median(lv))>3).sum())}")
print(f"lines >6 dB off median       : {int((np.abs(lv-np.median(lv))>6).sum())}")
print("worst 5 either way:")
for i in sorted(delivered, key=lambda i: -abs(delivered[i] - np.median(lv)))[:5]:
    s, g, d, p = notes[i]
    print(f"    #{i:<3} {delivered[i]:6.1f} dBFS (raw {s:6.1f}, gain x{g:.2f}, peak x{g*0+ p:.3f})  {d:.2f}s  {fit[i]['text'][:26]}")
peaks = np.array([v[3] for v in notes.values()])
print(f"delivered peak max           : {20*np.log10(peaks.max()):.1f} dBFS   over -1: {int((peaks>0.891).sum())}")

# ------------------------------------------- how much of the old voice is left
print()
print("=== original voice left in the mix ===")
vox, rate_v = read_wav(JOB / "stems_out" / "vocals.wav")
bed, _ = read_wav(JOB / "stems_out" / "no_vocals.wav")
dubbed = sorted(JOB.glob("dubbed_full*.wav")) or sorted(JOB.glob("*.wav"))
out, rate_o = read_wav(dubbed[-1])
print(f"mix file: {dubbed[-1].name}")
n = min(len(vox), len(bed), len(out))
hop = int(rate_o * 0.02)
frames = n // hop
def env(x):
    return np.sqrt((x[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1) + 1e-12)
v, b, o = env(vox[: n]), env(bed[: n]), env(out[: n])
voiced = v > 10 ** (-40 / 20)                     # the original had speech here
# windows where nobody is now speaking Chinese: the dub track is the whole film, so
# subtract only where a Chinese line plays
mask = np.ones(frames, dtype=bool)
for i, f in fit.items():
    a, e = int(f["start"] / 0.02), int((f["start"] + f["seconds"]) / 0.02)
    mask[a:min(e, frames)] = False
pockets = voiced & mask
print(f"frames with old speech, no dub: {pockets.sum()} ({pockets.sum()*0.02:.0f} s)")
if pockets.sum():
    # Compared against the bed, not against the old voice: the bed is what the
    # output is supposed to be there, so 0 dB means nothing of the old voice
    # survived. Dividing by the vocal instead reports the bed's own level and
    # reads as if the old voice were still audible.
    delta = 20 * np.log10(o[pockets] / np.maximum(b[pockets], 1e-12))
    print(f"    output minus bed            : median {np.median(delta):+.2f} dB "
          f"(p95 {np.percentile(delta, 95):+.2f} dB)")
    print(f"    (0 dB = old voice gone; +6 dB = it is back)")
