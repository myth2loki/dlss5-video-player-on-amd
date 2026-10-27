"""Measure a subtitle file the way a viewer would, without trusting its author.

The generator enforces a minimum duration and a maximum width, so measuring those
from the generator's own counters would only prove the code agrees with itself.
This reads the finished `.ass` - the same file that goes into the container - and
counts what is actually there.

Thresholds are the ones in docs/SUBTITLE-AUDIO-METRICS-zh.md, so the numbers this
prints drop straight into that table.
"""
import os
import re
from pathlib import Path

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
SUBS = Path(os.environ.get("DUB_SUBTITLES", JOB / "subtitles.ass"))

READING = 9.0       # characters a second a viewer can absorb
READING_HARD = 12.0
WIDTH = 32          # half-width units, a full-width character counting two
MIN_DUR = 0.80
GAP = 0.08


def stamp(text):
    hours, minutes, seconds = text.split(":")
    return int(hours) * 3600 + int(minutes) * 60 + float(seconds)


def units(text):
    return sum(2 if ord(ch) > 0x2E7F else 1 for ch in text)


def read(path):
    cues = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.startswith("Dialogue:"):
            continue
        parts = raw.split(",", 9)
        rows = parts[9].strip().split("\\N")
        text = "".join(rows)
        cues.append({"start": stamp(parts[1]), "end": stamp(parts[2]),
                     "rows": rows, "chars": len(re.sub(r"[\s，。！？…、,.!?;：\"]", "", text))})
    return cues


cues = read(SUBS)
if not cues:
    raise SystemExit(f"no Dialogue lines in {SUBS}")

# ASS timestamps carry hundredths, so 7.80 - 7.00 arrives as 0.7999999999999998.
# Rounding to the format's own precision is what keeps a compliant cue from being
# counted as a short one - seven of them were, before this.
durations = [round(c["end"] - c["start"], 2) for c in cues]
speeds = [c["chars"] / d for c, d in zip(cues, durations) if d > 0]
gaps = [cues[i + 1]["start"] - cues[i]["end"] for i in range(len(cues) - 1)]
overlaps = [g for g in gaps if g < 0]
widths = [units(row) for c in cues for row in c["rows"]]
slow = [s for s in speeds if s > READING]
hard = [s for s in speeds if s > READING_HARD]
wide = [w for w in widths if w > WIDTH]
short = [d for d in durations if d < MIN_DUR]
tight = [g for g in gaps if 0 <= g < GAP]

speeds_sorted = sorted(speeds)


def pct(values, fraction):
    return values[min(len(values) - 1, int(len(values) * fraction))] if values else 0.0


print(f"cues            : {len(cues)}")
print(f"duration        : median {sorted(durations)[len(durations) // 2]:.2f} s  "
      f"min {min(durations):.2f}  max {max(durations):.2f}")
print(f"reading speed   : p50 {pct(speeds_sorted, 0.50):.1f}  p95 {pct(speeds_sorted, 0.95):.1f}  "
      f"max {max(speeds):.1f} chars/s   (limit {READING}, hard {READING_HARD})")
print(f"  over {READING}      : {len(slow)} ({len(slow) / max(1, len(speeds)):.0%})"
      f"   over {READING_HARD}: {len(hard)}")
print(f"row width       : max {max(widths)} units   (limit {WIDTH})   over: {len(wide)} rows")
print(f"under {MIN_DUR} s     : {len(short)} cue(s)"
      + (f"   shortest {min(short):.2f} s" if short else ""))
print(f"gap under {GAP} s   : {len(tight)}   overlapping: {len(overlaps)}")
if short:
    print("  short cues: " + ", ".join(f"{d:.2f}" for d in sorted(short)))

print("\nslowest:")
for speed, cue in sorted(zip(speeds, cues), key=lambda pair: -pair[0])[:3]:
    print(f"  {speed:5.1f} chars/s over {cue['end'] - cue['start']:.2f} s  "
          f"{''.join(cue['rows'])[:34]}")
