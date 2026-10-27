"""Apply the voice-identity correction to an existing lines.json without re-running
step1 (no whisper, no re-embedding, no re-timing).

step1_align now calls voice.repair_lines itself, so a fresh run reproduces this. This
script exists for the targeted re-label of an already-built job, where only the speaker
field needs to change and every other field -- timing, embedding, translation -- must be
preserved. It loads the separated stems at their native rate (floors are in Hz, so the
rate does not matter) and writes the corrected lines.json back.
"""
import json
import os
import sys

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import voice  # noqa: E402

ROOT = os.environ.get("DUB_ROOT", "F:/tmp/s2st/jobs/t5")
VOCALS = os.environ.get("DUB_VOCALS", os.path.join(ROOT, "stems_out/vocals.wav"))
BED = os.environ.get("DUB_BED", os.path.join(ROOT, "stems_out/no_vocals.wav"))
LINES = os.path.join(ROOT, "work/lines.json")


def main():
    lines = json.load(open(LINES, encoding="utf-8"))
    vocal, vrate = sf.read(VOCALS, always_2d=True)
    bed, brate = sf.read(BED, always_2d=True)
    assert vrate == brate and len(vocal) == len(bed), "vocal and bed must match"
    vocal = vocal.mean(axis=1)
    bed = bed.mean(axis=1)

    print(f"correcting {len(lines)} lines against on-screen pitch")
    changes = voice.repair_lines(
        lines, vocal, bed, vrate, voices=("tom", "celia", "robot", "elder"))
    json.dump(lines, open(LINES, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"\nwrote {LINES}")
    if changes:
        print("changed:")
        for i, before, after in changes:
            print(f"  #{i:2d} {before} -> {after}")
    else:
        print("no changes")


if __name__ == "__main__":
    main()
