"""Align each line's assigned speaker with the gender (and, within male, the
register) actually audible on screen.

The diariser (ECAPA, in step1_align) and the hand truth table get *who* right on the
main characters but misfire on two things pitch settles cleanly:

  * short, weakly-scored lines where the embedding has no real opinion -- a 0.4 s
    "Be honest." lands on whichever voice the noise picked, and a scene of them came
    out female while a man is on screen;
  * the old man, whose register sits a full octave below the young lead. Both are
    male, so the embedding (and the truth table) fold him into "tom", and the clone
    then speaks him an octave too high.

Pitch is measured with YIN (tools/dub-metrics/pitch.py) on the frames where the voice
clearly dominates the music -- the same filter audit_voice_identity.source_reading uses.
Measuring it here the same way means the correction and the gate cannot disagree.

Three moves, each guarded so it cannot invent an error:

0. Truth override. A hand-verified label the embedding overrode is restored. This is
   how the old man enters: his lines carry truth="elder" and would otherwise be parked
   on Tom by the centroid. Film-specific, like the rest of the truth table.

1. Gender correction, single line, bidirectional, guarded by weak(). On-screen female
   parked on a male voice, and the diariser was not confident -> celia. Mirror: on-screen
   male parked on a female voice, diariser weak -> tom, or elder when the register is low.
   weak() is the guard: a line the embedding is sure of is never overridden, because a
   lone pitch trace in overlapping dialogue can be the other actor's voice bleeding
   through. That is exactly the "I'm not freaked out" exchange -- Tom speaks over Celia
   there and the trace flips female; overriding would put a woman's voice where Tom is.

2. Neighbour sweep for still-unlabelled lines (unchanged from before).
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "dub-metrics"))
import pitch  # noqa: E402  (pure-numpy YIN; no parselmouth)

# Gender bands and the clean-frame filter mirror audit_voice_identity, so a correction
# and the gate that checks it cannot disagree.
FEMALE = 195.0       # on-screen floor >= this reads female
MALE = 165.0         # on-screen floor <= this reads male; between the two is unstated
ELDER_FLOOR = 115.0  # a *male* line below this is the old man, not the young lead
MIN_FRAMES = 20      # fewer clean voiced frames -> the reading is not believed
SEPARATION = 9.0     # dB the voice must sit above the bed before a frame is kept
MUSIC_FLOOR = 0.03   # ... and the bed must be quiet in absolute terms

FEMALE_VOICES = ("celia",)
DEFAULT_MALE = "tom"
ELDER = "elder"


def _frame_rms(samples, count, size, hop):
    """RMS of the frame each F0 reading was taken from, aligned to the YIN grid."""
    squares = np.concatenate(([0.0], np.cumsum(np.asarray(samples, dtype=np.float64) ** 2)))
    starts = np.arange(count) * hop
    ends = np.minimum(starts + size, len(samples))
    starts = np.minimum(starts, len(samples))
    return np.sqrt(np.maximum(squares[ends] - squares[starts], 0.0) / np.maximum(ends - starts, 1))


def source_floor(vocal_seg, bed_seg, rate):
    """YIN pitch-floor of the on-screen voice, on frames where it beats the music.

    Returns (floor_or_None, voiced_frames). Identical filter to the audit's
    source_reading, so a line judged here is judged the same way at the gate.
    """
    track = pitch.yin_track(vocal_seg, rate)
    if not len(track):
        return None, 0
    size, hop = int(pitch.WINDOW * rate), int(pitch.HOP * rate)
    loud = _frame_rms(vocal_seg, len(track), size, hop)
    music = _frame_rms(bed_seg, len(track), size, hop)
    keep = (track > 0) & (loud > music * 10 ** (SEPARATION / 20)) & (music < MUSIC_FLOOR)
    return pitch.floor_of(track[keep])


def gender_of(floor):
    if floor is None:
        return None
    if floor >= FEMALE:
        return "f"
    if floor <= MALE:
        return "m"
    return None


def weak(line):
    """True when the embedding did not really pick a speaker."""
    scores = line.get("scores") or {}
    best = max(scores.values()) if scores else 0.0
    return best < 0.35 or float(line.get("margin", 0.0)) < 0.15


def repair_lines(lines, vocal, bed, sr, voices=(), log=print):
    """Re-label lines whose on-screen gender / male register contradicts the
    assignment. Returns (index, before, after) changes.

    `vocal` and `bed` are the separated stems at `sr` (any rate -- floors come back in
    Hz). `voices` is the set of speakers that have, or will have, a reference; "elder"
    is always accepted as a target so step11 can then build its clip.
    """
    vocal = np.asarray(vocal, dtype=np.float32).reshape(-1)
    bed = np.asarray(bed, dtype=np.float32).reshape(-1)
    changes = []
    for line in lines:
        if line.get("skip"):
            continue
        a, b = int(line["start"] * sr), int(line["end"] * sr)
        seg, bseg = vocal[a:b], bed[a:b]
        floor, n = source_floor(seg, bseg, sr)
        line["pitch"] = {
            "floor": round(floor, 1) if floor else None,
            "n": int(n),
            "gender": gender_of(floor),
        }
        spk = line["speaker"]
        truth = line.get("truth")

        # 0. Truth override: a hand-verified label the embedding overrode is restored.
        #    This is how the old man enters -- his lines carry truth="elder" and would
        #    otherwise be parked on Tom by the centroid.
        if truth and truth in voices and truth != spk:
            _reassign(line, truth, changes, log)
            continue

        if floor is None or n < MIN_FRAMES:
            continue
        src_g = gender_of(floor)
        cur_g = "f" if spk in FEMALE_VOICES else "m" if spk in voices else None

        # 1. Gender correction -- only when the diariser was not confident, so a pitch
        #    trace contaminated by the other actor's overlapping voice cannot override a
        #    real assignment (the "I'm not freaked out" trap).
        if src_g and cur_g and src_g != cur_g:
            if truth and truth == spk:
                continue
            if not weak(line):
                continue
            target = (FEMALE_VOICES[0] if src_g == "f"
                      else (ELDER if floor < ELDER_FLOOR else DEFAULT_MALE))
            if target != spk and target in voices:
                _reassign(line, target, changes, log)
            continue

    changes += _neighbour_sweep(lines, voices, log)
    return changes


def _reassign(line, target, changes, log):
    before = line["speaker"]
    line["speaker"] = target
    line["speaker_before"] = before
    changes.append((line["index"], before, target))
    log(f"  #{line['index']:2d} {before} -> {target} "
        f"(pitch floor {line['pitch']['floor']} Hz, on-screen {line['pitch']['gender']})")


def _neighbour_sweep(lines, voices, log):
    """Give every still-unlabelled line the voice of the nearest labelled line.

    step11 cannot leave a label without a reference, so "unknown" borrows whichever
    voice carries the most lines -- on this film that is the female lead, and every
    too-short-to-embed interjection came out female. A line that is 0.4 s long has no
    timbre to measure, but it does sit inside somebody's turn, so its neighbour is the
    better guess.
    """
    known = [line for line in lines if line["speaker"] in voices]
    if not known:
        return []
    changes = []
    for line in lines:
        if line["speaker"] in voices:
            continue
        midpoint = 0.5 * (line["start"] + line["end"])
        nearest = min(known, key=lambda other: abs(
            0.5 * (other["start"] + other["end"]) - midpoint))
        before = line["speaker"]
        line["speaker"] = nearest["speaker"]
        line["speaker_before"] = before
        changes.append((line["index"], before, nearest["speaker"]))
        log(f"  #{line['index']} {before} -> {nearest['speaker']} "
            f"(nearest labelled line #{nearest['index']})")
    return changes
