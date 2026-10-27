"""Measure the two things a dub changes: the subtitle track and the new audio track.

Numbers come out of the job directory, not out of a screenshot: the cue list the
subtitles are written from, the fit results that say what each line was allowed,
and the per-line wavs that were mixed in.
"""
import json
import os
import re
import unicodedata
import wave
from pathlib import Path

import numpy as np

# DUB_JOB points at the job directory the dubbing chain wrote; everything below
# is read out of it rather than out of the finished file, because most of what
# goes wrong (a cue that outruns the voice, a line that had to be cut) leaves no
# trace in the container.
JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK, DUB = JOB / "work", JOB / "dub_auto"


def load(name):
    return json.load(open(WORK / name, encoding="utf-8"))


def width(text):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def cue_text(row):
    """The zh line, keeping only what a viewer reads (no markers)."""
    return "".join(c for c in row.get("zh") or "" if c not in "\ufeff")


def read_wav(path):
    with wave.open(str(path)) as w:
        rate, width_, channels = w.getframerate(), w.getsampwidth(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    if width_ != 2:
        raise SystemExit(f"unexpected sample width {width_}: {path}")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data, rate


def db(x):
    return 20 * np.log10(max(float(x), 1e-9))


lines = {r["index"]: r for r in load("lines.json")}
zh = {r["index"]: r for r in load("zh_final.json")}
fit = {r["index"]: r for r in load("fit_auto.json")}
mix = {r["index"]: r for r in load("mix_auto.json")}

# ---------------------------------------------------------------- subtitles
cues = []
for index, row in sorted(lines.items()):
    text = cue_text(zh.get(index, {}))
    start, end = row["start"], row["end"]
    dur = end - start
    cues.append({"index": index, "start": start, "end": end, "dur": dur,
                 "text": text, "chars": len(text), "width": width(text),
                 "zh": index in fit})

print("=== subtitles ===")
print(f"cues in the source track      : {len(cues)}")
print(f"cues carrying a translation   : {sum(1 for c in cues if c['zh'])}")

durs = np.array([c["dur"] for c in cues])
print(f"cue duration  min/median/max  : {durs.min():.2f} / {np.median(durs):.2f} / {durs.max():.2f} s")
print(f"cues under 1.00 s             : {int((durs < 1.0).sum())}  (<0.80 s: {int((durs < 0.8).sum())})")

speeds = [(c["chars"] / c["dur"], c) for c in cues if c["chars"] and c["dur"] > 0]
rates = np.array([s for s, _ in speeds])
print(f"reading speed  median/p95/max : {np.median(rates):.1f} / {np.percentile(rates, 95):.1f} / {rates.max():.1f} chars/s")
print(f"cues over 9 chars/s           : {int((rates > 9).sum())}   over 12: {int((rates > 12).sum())}")
over = sorted(speeds, key=lambda s: -s[0])[:5]
for s, c in over:
    print(f"    #{c['index']:<3} {s:5.1f} c/s  {c['dur']:.2f}s  {c['text']}")

widths = np.array([c["width"] for c in cues if c["chars"]])
print(f"line width  median/max        : {np.median(widths):.0f} / {widths.max():.0f} (CJK=2)")
print(f"cues wider than 32 units      : {int((widths > 32).sum())}")
for c in sorted([c for c in cues if c["chars"]], key=lambda c: -c["width"])[:4]:
    print(f"    #{c['index']:<3} {c['width']:3d}u  {c['text']}")

gaps = [cues[i + 1]["start"] - cues[i]["end"] for i in range(len(cues) - 1)]
print(f"gaps  min/median/max          : {min(gaps):.2f} / {np.median(gaps):.2f} / {max(gaps):.2f} s")
print(f"gaps under 0.08 s             : {sum(1 for g in gaps if g < 0.08)}   overlapping: {sum(1 for g in gaps if g < 0)}")

# how far a cue's picture text is from the dubbed voice that now speaks it
offsets = [(fit[i]["start"] - lines[i]["start"], i) for i in fit]
off = np.array([o for o, _ in offsets])
print(f"dub start vs cue start        : median {np.median(off):+.2f} s, max {np.abs(off).max():.2f} s")
print(f"lines shifted over 0.30 s     : {int((np.abs(off) > 0.30).sum())}")

# ------------------------------------------------------------------- voice
print()
print("=== new audio track ===")
ratios = np.array([r["ratio"] for r in fit.values()])
tempos = np.array([r["tempo"] for r in fit.values()])
stages = {}
for r in fit.values():
    stages[r["stage"]] = stages.get(r["stage"], 0) + 1
print(f"lines fitted                  : {len(fit)}")
print(f"stage split                   : {stages}")
print(f"ratio (voice/window) max      : {ratios.max():.2f}   over 1.00: {int((ratios > 1.0).sum())}")
print(f"tempo used   >1.00 / 1.10 / 1.25 / 1.50 : "
      f"{int((tempos > 1.0).sum())} / {int((tempos > 1.10).sum())} / {int((tempos > 1.25).sum())} / {int((tempos > 1.50).sum())}")
print(f"tempo max                     : {tempos.max():.2f}   median {np.median(tempos):.2f}")

wavs = sorted(DUB.glob("zh*.wav"))
levels, peaks, leads, tails, durs_wav = {}, {}, {}, {}, {}
for path in wavs:
    index = int(re.sub(r"\D", "", path.stem) or -1)
    if index not in fit:
        continue
    data, rate = read_wav(path)
    durs_wav[index] = len(data) / rate
    # 20 ms frames: everything that matters here is longer than that
    hop = int(rate * 0.02)
    frames = len(data) // hop
    if frames < 1:
        continue
    rms = np.sqrt((data[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1) + 1e-12)
    loud = 20 * np.log10(rms + 1e-9)
    # speech level: the loud half of the frames, so pauses do not drag it down
    levels[index] = float(np.percentile(loud, 75))
    peaks[index] = db(np.abs(data).max())
    silent = np.where(loud > -45)[0]
    if len(silent):
        leads[index] = silent[0] * hop / rate
        tails[index] = (frames - 1 - silent[-1]) * hop / rate

lv = np.array(list(levels.values()))
print(f"lines measured                : {len(lv)}")
print(f"speech level  p05/median/p95  : {np.percentile(lv, 5):.1f} / {np.median(lv):.1f} / {np.percentile(lv, 95):.1f} dBFS")
print(f"spread (p95-p05)              : {np.percentile(lv, 95) - np.percentile(lv, 5):.1f} dB")
print(f"lines off median by >3 dB     : {int((np.abs(lv - np.median(lv)) > 3).sum())}")
lo = sorted(levels.items(), key=lambda kv: kv[1])[:3]
hi = sorted(levels.items(), key=lambda kv: -kv[1])[:3]
print(f"    quietest {[ (i, round(v,1)) for i,v in lo ]}")
print(f"    loudest  {[ (i, round(v,1)) for i,v in hi ]}")

pk = np.array(list(peaks.values()))
print(f"line peak max                 : {pk.max():.1f} dBFS   over -1 dBFS: {int((pk > -1).sum())}")
lead = np.array(list(leads.values()))
tail = np.array(list(tails.values()))
print(f"leading silence  p95/max      : {np.percentile(lead, 95)*1000:.0f} / {lead.max()*1000:.0f} ms")
print(f"trailing silence p95/max      : {np.percentile(tail, 95)*1000:.0f} / {tail.max()*1000:.0f} ms")

drift = [(abs(durs_wav[i] - fit[i]["seconds"]), i) for i in durs_wav if i in fit]
dl = np.array([d for d, _ in drift])
print(f"wav vs fit duration  max      : {dl.max()*1000:.0f} ms   over 50 ms: {int((dl > 0.05).sum())}")

# where the mix log says it gave up
try:
    log = (JOB.parent.parent / f"jobs_{JOB.name}_fit5.log").read_text(encoding="utf-8", errors="replace")
except OSError:
    log = ""
print()
for label in ("重叠", "掐尾", "under", "相加"):
    for m in re.findall(rf"^.*{label}.*$", log, re.M):
        print("mix:", m.strip()[:150])
