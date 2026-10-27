"""Where the timing comes from, and how much room is actually in each line.

Four different things get called "inaccurate": the cue is in the wrong place,
the window is too small, the words are wrong, or the wrong person says them.
This measures the first two, because those are upstream of everything else -
every downstream concession (shift, tempo, rewrite, cut) is a patch over a
window that was mis-measured or too small to begin with.
"""
import json
import os
from pathlib import Path

import numpy as np

import wavio

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK = JOB / "work"
load = lambda n: json.load(open(WORK / n, encoding="utf-8"))


read_wav = wavio.read_wav
lines = {r["index"]: r for r in load("lines.json")}
fit = {r["index"]: r for r in load("fit_auto.json")}
zh = {r["index"]: (r["zh"] or "") for r in load("zh_final.json")}

vox, rate = read_wav(JOB / "stems_out" / "vocals.wav")
hop = int(rate * 0.01)
frames = len(vox) // hop
env = np.sqrt((vox[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1) + 1e-12)
loud = 20 * np.log10(env + 1e-9)
speech = loud > -42.0

# Close gaps shorter than 0.12 s: breath inside a sentence is not a boundary.
run, i = [], 0
while i < frames:
    if speech[i]:
        j = i
        while j < frames and speech[j]:
            j += 1
        run.append([i, j])
        i = j
    else:
        i += 1
merged = []
for seg in run:
    if merged and (seg[0] - merged[-1][1]) * 0.01 < 0.12:
        merged[-1][1] = seg[1]
    else:
        merged.append(seg)
segments = [(a * 0.01, b * 0.01) for a, b in merged if (b - a) * 0.01 > 0.05]
print(f"english speech segments in the dry vocal stem: {len(segments)}")
print(f"cues in lines.json                           : {len(lines)}")

# Match each cue to the segments it overlaps, and take the union of those: a
# long cue may hold several breaths, and picking the single best-overlapping
# segment reports one of them as if it were the whole line.
start_err, end_err, missed, spans = [], [], 0, []
for i, row in lines.items():
    touching = [(a, b) for a, b in segments if min(b, row["end"]) - max(a, row["start"]) > 0.05]
    if not touching:
        missed += 1
        continue
    real_start, real_end = min(a for a, _ in touching), max(b for _, b in touching)
    start_err.append(row["start"] - real_start)
    end_err.append(row["end"] - real_end)
    spans.append(row["end"] - row["start"])

se, ee = np.array(start_err), np.array(end_err)
print(f"cues with no speech under them               : {missed}")
print(f"cue start minus real speech start  median / share within 0.10 s: "
      f"{np.median(se):+.3f} s / {np.mean(np.abs(se) < 0.10)*100:.0f}%")
print(f"cue end   minus real speech end    median / share within 0.10 s: "
      f"{np.median(ee):+.3f} s / {np.mean(np.abs(ee) < 0.10)*100:.0f}%")
print("  (negative end error = the cue stops before the voice does, so the window")
print("   is smaller than the English it has to replace)")

# Whole-film coverage: what the cue list keeps and what it wastes.
inside = np.zeros(frames, dtype=bool)
for row in lines.values():
    inside[max(0, int(row["start"] / 0.01)):min(frames, int(row["end"] / 0.01))] = True
print(f"speech covered by some cue                   : "
      f"{speech[inside].sum()/max(1, speech.sum())*100:.1f}% of speaking time")
print(f"cue time with nobody speaking under it       : "
      f"{(~speech[inside]).sum()/max(1, inside.sum())*100:.1f}% of cue time "
      f"({(~speech[inside]).sum()*0.01:.0f} s)")

print()
print("=== how much room each window really has ===")
rows = []
for i, f in fit.items():
    text = f["text"]
    win = f["window"]
    rows.append((i, win, len(text), len(text) / win if win else 99, f["seconds"] / win if win else 99))
r = np.array([[x[1], x[2], x[3]] for x in rows])
print(f"window  p05/median/p95 : {np.percentile(r[:,0],5):.2f} / {np.median(r[:,0]):.2f} / {np.percentile(r[:,0],95):.2f} s")
print(f"chars    median        : {np.median(r[:,1]):.0f}")
print(f"chars/window  median/max: {np.median(r[:,2]):.1f} / {r[:,2].max():.1f}")
print()
print("lines that cannot fit even at 1.50x (needs > 12 chars/s after tempo):")
for i, win, n, rate_needed, ratio in sorted(rows, key=lambda x: -x[3]):
    if rate_needed / 1.5 > 12:
        print(f"    #{i:<3} {win:.2f}s  {n:2d} 字  {rate_needed:5.1f} 字/秒  「{fit[i]['text']}」")

print()
print("=== the room next to each line (gap to the neighbours) ===")
order = sorted(fit)
slack_before, slack_after = [], []
for a, b in zip(order, order[1:]):
    gap = lines[b]["start"] - lines[a]["end"]
    slack_after.append((gap, a))
gaps = np.array([g for g, _ in slack_after])
print(f"gap between consecutive cues  median/p95: {np.median(gaps):.2f} / {np.percentile(gaps,95):.2f} s")
print(f"cues with < 0.15 s of room after them     : {int((gaps<0.15).sum())}")
print(f"cues with > 1.0 s of room after them      : {int((gaps>1.0).sum())}")

print()
print("=== who is speaking, per line ===")
by_voice = {}
for f in fit.values():
    by_voice.setdefault(f["speaker"], []).append(f["index"])
for voice, idx in sorted(by_voice.items()):
    print(f"    {voice:<8} {len(idx):3d} lines")
short = [(f["index"], f["window"]) for f in fit.values() if f["window"] < 0.8]
print(f"lines under 0.8 s (where the speaker test is weakest): {len(short)}")
