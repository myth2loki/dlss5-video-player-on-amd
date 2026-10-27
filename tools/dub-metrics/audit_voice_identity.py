"""Is the voice coming out of the speaker the one that belongs to the person on screen?

Nothing in the chain checked this. `voice.py` decides who is talking, and it can only
prove a line *is male* -- it cannot tell two men apart, and it works per turn, so one
wrong label in a mixed turn survives into the mix. The cloner then does what it is told,
and the first evidence of a mistake is somebody watching the film.

Three readings per line, all with the same estimator (see pitch.py):

    on screen  the source line, cut from the vocal stem
    chosen     the reference clip of the label the pipeline picked
    delivered  the Chinese clip that goes into the mix

Comparing them in that order splits a mismatch by stage:

    chosen != on screen   the diariser put the line on the wrong voice
    delivered != chosen   the clip does not sound like the reference it was cloned from
                          (a stale file from an earlier run, or the cloner drifting)
    delivered != on screen  what the audience actually hears

The threshold is 7 semitones -- over half an octave. Two takes of the same speaker stay
well inside that; a change of speaker, or a change of gender, does not come close.

The on-screen reading is taken only on frames where the vocal stem stands at least 6 dB
above the backing track. Judging the whole line instead throws away most of the film: the
stem keeps a lot of music, a tonally stable pad tracks perfectly well as F0, and on this
material only 27 of 72 lines have a clean average -- while nearly every line has a few
seconds of it. Frames that fail are dropped, the rest are kept; under 20 kept frames the
line is reported as unmeasured rather than guessed at.

Run:  DUB_JOB=... python tools/dub-metrics/audit_voice_identity.py
Exit code 1 when any line is heard wrong, so it can gate a build.
"""
import json
import os
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pitch                                   # noqa: E402
import wavio                                   # noqa: E402

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK = JOB / "work"
DUB = Path(os.environ.get("DUB_CLIPS", JOB / "dub_auto"))
REFS = Path(os.environ.get("DUB_REFS", JOB / "refs_auto"))
VOCALS = Path(os.environ.get("DUB_VOCALS", JOB / "stems_out/vocals.wav"))
BED = Path(os.environ.get("DUB_BED", JOB / "stems_out/no_vocals.wav"))

OFF = 7.0          # semitones; half an octave is a different person
SEPARATION = 9.0   # dB of vocal over backing, per frame, before that frame is believed
MUSIC_FLOOR = 0.03 # ... and the backing has to be quiet in absolute terms too, or a loud
                   # cue the splitter left behind is itself stable enough to track as F0
MIN_FRAMES = 20    # 0.2 s of usable voiced audio
MALE = 165.0       # p10 below this reads male... calibrated on this film's own refs
FEMALE = 195.0     # ... above this, female; in between, unstated


def gender(hz):
    if hz is None:
        return None
    return "m" if hz <= MALE else ("f" if hz >= FEMALE else None)


def frame_rms(samples, count, size, hop):
    """RMS of the frame each F0 reading was taken from, on the same grid."""
    squares = np.concatenate(([0.0], np.cumsum(np.asarray(samples, dtype=np.float64) ** 2)))
    starts = np.arange(count) * hop
    ends = np.minimum(starts + size, len(samples))
    starts = np.minimum(starts, len(samples))
    return np.sqrt(np.maximum(squares[ends] - squares[starts], 0.0) / np.maximum(ends - starts, 1))


def source_reading(vocal, backing, rate):
    """F0 floor of the source line, measured only where the voice outweighs the music."""
    track = pitch.yin_track(vocal, rate)
    if not len(track):
        return None, 0, 0.0
    size, hop = int(pitch.WINDOW * rate), int(pitch.HOP * rate)
    loud = frame_rms(vocal, len(track), size, hop)
    music = frame_rms(backing[:len(vocal)], len(track), size, hop)
    keep = (track > 0) & (loud > music * 10 ** (SEPARATION / 20)) & (music < MUSIC_FLOOR)
    floor, frames = pitch.floor_of(track[keep])
    share = float(keep.sum()) / max(1, int((track > 0).sum()))
    return floor, frames, share


def main():
    lines = {row["index"]: row for row in
             json.load(open(WORK / "lines.json", encoding="utf-8"))}
    try:
        fit = {row["index"]: row for row in
               json.load(open(WORK / "fit_auto.json", encoding="utf-8"))}
    except FileNotFoundError:
        fit = {}

    vocals, vocal_rate = wavio.read_wav(VOCALS)
    backing, backing_rate = wavio.read_wav(BED)

    print("reference clips -- the voice each label is supposed to sound like")
    reference = {}
    for path in sorted(REFS.glob("*.wav")):
        clip, rate = wavio.read_wav(path)
        floor, frames = pitch.pitch_floor(clip, rate)
        reference[path.stem] = floor
        print(f"  {path.stem:8s} {floor if floor is None else round(floor):>4} Hz  "
              f"{gender(floor) or 'unclear':7s} {frames} voiced frames")

    rows = []
    for index in sorted(lines):
        line = lines[index]
        speaker = line["speaker"]
        start, end = line["start"], line["end"]
        vocal = vocals[int(start * vocal_rate):int(end * vocal_rate)]
        bed = backing[int(start * backing_rate):int(end * backing_rate)]
        source, source_frames, share = source_reading(vocal, bed, vocal_rate)
        trusted = source_frames >= MIN_FRAMES

        clip_path = DUB / f"zh{index:02d}.wav"
        delivered, delivered_frames = (None, 0)
        if clip_path.exists():
            clip, rate = wavio.read_wav(clip_path)
            delivered, delivered_frames = pitch.pitch_floor(clip, rate)

        rows.append({
            "index": index, "speaker": speaker, "start": start, "state": "ok",
            "source": source if trusted else None,
            "delivered": delivered if delivered_frames >= MIN_FRAMES else None,
            "chosen": reference.get(speaker),
            "clean_share": share, "trusted": trusted,
            "source_text": line.get("text", ""),
            "text": fit.get(index, {}).get("text", ""),
            "has_clip": clip_path.exists(),
        })

    heard, labelled, cloned = [], [], []
    for row in rows:
        source, delivered, chosen = row["source"], row["delivered"], row["chosen"]
        if source and delivered:
            gap = pitch.semitones(delivered, source)
            row["heard"] = gap
            if abs(gap) >= OFF:
                heard.append(row)
        if source and chosen:
            gap = pitch.semitones(source, chosen)
            row["label"] = gap
            if abs(gap) >= OFF:
                labelled.append(row)
        if delivered and chosen:
            gap = pitch.semitones(delivered, chosen)
            row["voice"] = gap
            if abs(gap) >= OFF:
                cloned.append(row)

    measurable = [r for r in rows if r["source"] and r["delivered"]]
    unmeasured = [r for r in rows if r["source"] is None]
    missing = [r for r in rows if not r["has_clip"]]
    flips = [r for r in measurable if gender(r["source"]) and gender(r["delivered"])
             and gender(r["source"]) != gender(r["delivered"])]

    print(f"\nlines                : {len(rows)}   with a Chinese clip: "
          f"{len(rows) - len(missing)}"
          + (f"   without: {', '.join('#' + str(r['index']) for r in missing)}"
             if missing else ""))
    print(f"on screen measurable : {len(measurable)}  (the rest: fewer than "
          f"{MIN_FRAMES} frames with the voice {SEPARATION:.0f} dB over a quiet bed)")
    print(f"heard (delivered vs screen) : {len(heard)} wrong of {len(measurable)}"
          f"   {len(heard) / max(1, len(measurable)):.0%}"
          f"   of which gender flips: {len(flips)}")
    print(f"label (chosen vs screen)    : {len(labelled)} wrong of "
          f"{len([r for r in rows if r['source'] and r['chosen']])}")
    print(f"voice (delivered vs chosen) : {len(cloned)} wrong of "
          f"{len([r for r in rows if r['delivered'] and r['chosen']])}")

    for title, group in (("heard wrong -- the audience hears this", heard),
                         ("label wrong -- the diariser picked the wrong voice", labelled),
                         ("voice wrong -- the clip is not its own reference", cloned)):
        if not group:
            continue
        print(f"\n{title}:")
        for r in sorted(group, key=lambda r: r["start"]):
            parts = []
            for label, key in (("screen", "source"), ("chosen", "chosen"),
                               ("delivered", "delivered")):
                parts.append(f"{label} {r[key]:.0f}" if r[key] else f"{label} -")
            heading = f"  #{r['index']:<3d} {r['speaker']:6s} {r['start']:7.2f}s  " + \
                      "  ".join(parts)
            if r.get("heard") is not None:
                heading += f"   ({r['heard']:+.1f} st heard)"
            if r in flips:
                heading += "   [m/f flip]"
            print(heading)
            if r["source_text"]:
                print(f"        {r['source_text'][:58]}")
            if r["text"]:
                print(f"        {r['text'][:58]}")

    print("\nper speaker (median p10, all readings):")
    for speaker in sorted({r["speaker"] for r in rows}):
        group = [r for r in rows if r["speaker"] == speaker]
        cells = []
        for key in ("source", "chosen", "delivered"):
            values = [r[key] for r in group if r[key]]
            cells.append(f"{np.median(values):5.0f}" if values else "    -")
        print(f"  {speaker:6s} n={len(group):3d}  screen {cells[0]}  chosen {cells[1]}"
              f"  delivered {cells[2]} Hz")

    if unmeasured:
        print("\nunmeasured (stem too weak to judge, not counted either way): "
              + ", ".join(f"#{r['index']}" for r in unmeasured))

    print(f"\ngate: {len(heard)} heard wrong (limit 0), {len(cloned)} voice wrong (limit 0)"
          f"   -- threshold {OFF:.0f} semitones, {2 ** (OFF / 12):.2f}x pitch")
    return 1 if (heard or cloned) else 0


if __name__ == "__main__":
    sys.exit(main())
