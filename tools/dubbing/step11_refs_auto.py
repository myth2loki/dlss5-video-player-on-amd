"""Pick each speaker's reference clip automatically, from the separation stems.

Hand-picking works for one film and not for the next one. What makes a good clone
reference is measurable, so it is measured here:

  separation -- vocal energy over background energy in that line. A line sitting on a
                loud music cue keeps the actor's timbre, but the clone inherits the
                cue's spectral tilt.
  continuity -- share of the line that is actually voiced. A line full of pauses gives
                the model long silences to interpolate through.
  identity   -- the speaker score margin from the diarizer. Weak assignment means the
                reference may contain the other actor.
  length     -- enough to model prosody, not so much that the clip drifts in level.

The result is one concatenated clip per speaker, ordered by delivery time so prosody
stays coherent, with every chosen line recorded so the pick can be audited.
"""

import json
import os
import pathlib
import sys

import numpy as np
import soundfile as sf

ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
WORK = ROOT / "work"
OUT = ROOT / "refs_auto"
VOCALS = os.environ.get("DUB_VOCALS", str(ROOT / "stems_out/vocals.wav"))
BED = os.environ.get("DUB_BED", str(ROOT / "stems_out/no_vocals.wav"))

FRAME = 0.02          # 20 ms energy frames
MIN_LINE = 1.2        # shorter lines carry too little timbre to be worth including
MAX_LINE = 6.0        # longer lines add nothing and dilute the clip
TARGET = 10.0         # aim for ~10 s of reference per speaker
HARD_CAP = 15.0       # Qwen3-TTS reference window
MIN_TOTAL = 6.0       # below this the clip cannot represent prosody; borrow the lead
FADE = 0.005
GAP = 0.12


def frame_energy(samples, rate):
    size = max(1, int(FRAME * rate))
    usable = len(samples) // size * size
    if usable < size:
        return np.zeros(0)
    frames = samples[:usable].reshape(-1, size)
    return np.sqrt(np.mean(frames ** 2, axis=1))


def silence_ratio(samples, rate):
    energy = frame_energy(samples, rate)
    if len(energy) == 0:
        return 1.0
    reference = float(np.percentile(energy, 90))
    if reference <= 1e-6:
        return 1.0
    return float(np.mean(energy < reference * 0.06))


def rms(samples):
    return float(np.sqrt(np.mean(samples ** 2))) if len(samples) else 0.0


def main():
    OUT.mkdir(exist_ok=True)
    rows = json.load(open(WORK / "lines.json", encoding="utf-8"))

    vocal, vrate = sf.read(VOCALS, always_2d=True)
    vocal = vocal.mean(axis=1)
    backing, _ = sf.read(BED, always_2d=True)
    backing = backing.mean(axis=1)

    candidates = {}
    for row in rows:
        if row.get("skip"):
            continue
        # Register every speaker up front: a role whose only line is too short to be a
        # reference still needs an entry, or it never reaches the borrow fallback.
        candidates.setdefault(row.get("speaker") or "unknown", [])
        length = row["end"] - row["start"]
        if not (MIN_LINE <= length <= MAX_LINE):
            continue
        start, end = int(row["start"] * vrate), int(row["end"] * vrate)
        chunk = vocal[start:end]
        bed = backing[start:end]
        if len(chunk) < int(0.5 * vrate):
            continue
        sep = rms(chunk) / (rms(bed) + 1e-6)
        cont = 1.0 - silence_ratio(chunk, vrate)
        margin = float(row.get("margin") or 0.0)
        # Length preference: peaks around 3.5 s, rolls off toward both ends.
        prefer = 1.0 - abs(length - 3.5) / 3.5
        score = (0.45 * min(sep / 4.0, 1.0) + 0.30 * cont
                 + 0.15 * min(max(margin, 0.0) / 0.25, 1.0) + 0.10 * max(prefer, 0.0))
        candidates.setdefault(row.get("speaker") or "unknown", []).append({
            "index": row["index"], "start": row["start"], "end": row["end"],
            "length": round(length, 2), "separation": round(sep, 2),
            "continuity": round(cont, 3), "margin": round(margin, 3),
            "score": round(score, 3), "text": row["text"][:44],
        })

    fade_n = int(FADE * vrate)
    gap = np.zeros(int(GAP * vrate))
    manifest = {}

    for speaker, pool in sorted(candidates.items()):
        pool.sort(key=lambda c: -c["score"])
        chosen, total = [], 0.0
        for item in pool:
            if total >= TARGET or total + item["length"] > HARD_CAP:
                continue
            chosen.append(item)
            total += item["length"] + GAP
        if not chosen:
            print(f"{speaker:8s} no eligible line, skipped")
            continue
        chosen.sort(key=lambda c: c["start"])

        total_len = sum(item["length"] for item in chosen)
        if total_len < MIN_TOTAL:
            # A 2.7 s clip models too little prosody to clone from reliably; the pipeline
            # already borrowed the lead voice by hand for minor roles, so do it here.
            print(f"{speaker:8s} only {total_len:.2f}s of material, will borrow")
            candidates[speaker] = []
            continue

        pieces = []
        for item in chosen:
            start = int(max(0.0, item["start"] - 0.06) * vrate)
            end = int(min(len(vocal) / vrate, item["end"] + 0.06) * vrate)
            chunk = vocal[start:end].copy()
            if len(chunk) > 2 * fade_n:
                ramp = np.linspace(0.0, 1.0, fade_n)
                chunk[:fade_n] *= ramp
                chunk[-fade_n:] *= ramp[::-1]
            pieces += [chunk, gap]
        joined = np.concatenate(pieces)

        peak = float(np.max(np.abs(joined))) or 1.0
        if peak > 0.95:
            joined = joined / peak * 0.95

        path = OUT / f"{speaker}.wav"
        sf.write(str(path), joined.astype(np.float32), vrate)
        manifest[speaker] = {
            "path": str(path), "seconds": round(len(joined) / vrate, 2),
            "chosen": chosen,
        }
        print(f"{speaker:8s} {len(joined)/vrate:5.2f}s from {len(chosen)} lines -> {path.name}")
        for item in chosen:
            print(f"    #{item['index']:2d} {item['length']:4.2f}s sep={item['separation']:5.2f} "
                  f"cont={item['continuity']:.2f} score={item['score']:.2f}  {item['text'][:38]}")

    json.dump(manifest, open(WORK / "refs_auto.json", "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)

    # Fallback: minor roles borrow the voice that carries the most dialogue, not the one
    # with the longest reference. A robot and an unlabelled line belong to whoever leads
    # the scene; picking by clip length handed them the other actor's voice.
    if manifest:
        lead = max((s for s in candidates if s in manifest),
                   key=lambda s: len(candidates[s]), default=None)
        if lead:
            for speaker in candidates:
                if speaker not in manifest:
                    print(f"{speaker:8s} insufficient material -> borrowing {lead}")
                    manifest[speaker] = {**manifest[lead], "borrowed_from": lead}
            json.dump(manifest, open(WORK / "refs_auto.json", "w", encoding="utf-8"),
                      ensure_ascii=False, indent=1)

    print(f"\nreferences: {len(manifest)} speakers")


if __name__ == "__main__":
    main()
