"""Pitch band veto: prove a line is male when the speaker embedding cannot.

step1_align labels a line by cosine against per-speaker centroids, and asks for a
0.15 margin. On this film 29 of 70 lines miss that margin, and the misses are all
0.4-1.0 s interjections -- "Now.", "Be honest.", "Be tender." -- where ECAPA's
absolute cosine drops to 0.03-0.17 and the winner is decided by noise. Four of those
came out as the female lead while the scene is a man talking to the robot, which is
audible in the finished dub.

Pitch cannot identify *which* speaker (two men share a range), but it can rule one out.
Measured with Praat's autocorrelation on the vocal stem:

    tom (male, truth lines)   voiced coverage 165-350 Hz  0.14 - 0.31
    celia (female, truth)                                0.63 - 0.88
    the 232-248 s block                                  0.00 - 0.37

A voice with no periodicity at all above 165 Hz is not the female lead, so the test is
one-sided: `hi_coverage` below VETO proves male, anything above it says nothing. That
asymmetry is the point -- it can only ever move a line onto the male voice, so a wrong
read costs the existing label rather than inventing a new one.

Parselmouth wraps Praat (MIT, ~3 MB) -- nothing like the torch dependency this engine
was built to avoid, and stage 1 is the only consumer.
"""

import numpy as np

# 165 Hz = the male/female split; 350 Hz keeps Praat from chasing F0 of a child voice.
LOW_BAND = (60.0, 165.0)
HIGH_BAND = (165.0, 350.0)
VETO = 0.40          # high-band coverage below this: this voice is male
TIME_STEP = 0.01


def _coverage(samples, sr, floor, ceiling):
    """(voiced fraction, median F0) of one band, via Praat autocorrelation."""
    import parselmouth

    sound = parselmouth.Sound(np.asarray(samples, dtype=np.float64),
                              sampling_frequency=int(sr))
    pitch = sound.to_pitch_ac(time_step=TIME_STEP, pitch_floor=floor,
                              pitch_ceiling=ceiling)
    values = pitch.selected_array["frequency"]
    voiced = values[values > 0]
    fraction = float(len(voiced) / max(1, len(values)))
    median = float(np.median(voiced)) if len(voiced) else float("nan")
    return fraction, median


def profile(samples, sr):
    """Pitch evidence for one clip of speech."""
    samples = np.asarray(samples, dtype=np.float32).reshape(-1)
    if len(samples) < int(0.15 * sr):
        return None
    low_cov, low_f0 = _coverage(samples, sr, *LOW_BAND)
    high_cov, high_f0 = _coverage(samples, sr, *HIGH_BAND)
    if low_cov + high_cov <= 0:
        return None
    return {
        "low_coverage": round(low_cov, 3),
        "low_f0": None if not np.isfinite(low_f0) else round(low_f0, 1),
        "high_coverage": round(high_cov, 3),
        "high_f0": None if not np.isfinite(high_f0) else round(high_f0, 1),
        "male": high_cov < VETO,
    }


# --- turn-level repair -------------------------------------------------------------
# The veto is applied per *turn*, not per line. A single 0.4 s clip ("Be honest.") can
# read high or low depending on where the sentence falls in it, but a scene is 6-16 s of
# one voice, and the block at 232-248 s reads high-band coverage 0.00-0.39 on all eight
# lines -- no female voice can do that across a whole scene.
#
# A turn is a run of lines separated by less than TURN_GAP. Silence is the only turn
# boundary this pipeline can see (there is no diarizer), and 6 s is well above the
# 1-3 s gaps inside a scene while still below the 7 s+ gaps between scenes here.
TURN_GAP = 6.0
# Voices that the veto may not rewrite a line *to*. Everything else is fair game,
# including "unknown": step3_tts borrows Tom's voice for it.
FEMALE_VOICES = ("celia",)
DEFAULT_MALE = "tom"


def weak(line):
    """True when the embedding did not really pick a speaker."""
    scores = line.get("scores") or {}
    best = max(scores.values()) if scores else 0.0
    return best < 0.35 or float(line.get("margin", 0.0)) < 0.15


def _turns(lines):
    turn = []
    for line in lines:
        if turn and line["start"] - turn[-1]["end"] > TURN_GAP:
            yield turn
            turn = []
        turn.append(line)
    if turn:
        yield turn


def repair_turns(lines, samples, sr, voices=(), log=print):
    """Relabel lines the pitch veto proves male, then sweep up what is still unlabelled.

    Only lines that landed on a female voice or on no voice at all are rewritten -- a
    line the embedding gave to another *male* label (the robot) is left alone, because
    pitch can never tell two male voices apart.

    Returns the changes as (index, before, after).
    """
    samples = np.asarray(samples, dtype=np.float32).reshape(-1)
    changes = []
    for turn in _turns(lines):
        profiled = []
        for line in turn:
            seg = samples[int(line["start"] * sr):int(line["end"] * sr)]
            evidence = profile(seg, sr)
            if evidence is not None:
                line["pitch"] = evidence
                profiled.append((line, evidence))
        if len(profiled) < 2 or not all(e["male"] for _, e in profiled):
            continue
        male = [line["speaker"] for line, _ in profiled
                if line["speaker"] not in FEMALE_VOICES and line["speaker"] in voices]
        if male:
            owner = max(set(male), key=male.count)
        elif DEFAULT_MALE in voices:
            owner = DEFAULT_MALE
        else:
            continue
        touched = []
        for line, _ in profiled:
            before = line["speaker"]
            if before in FEMALE_VOICES or before not in voices:
                line["speaker"] = owner
                if before != owner:
                    line["speaker_before"] = before
                    touched.append((line["index"], before, owner))
        if touched:
            log(f"  turn {turn[0]['start']:.1f}-{turn[-1]['end']:.1f}s, high-band "
                f"coverage {min(e['high_coverage'] for _, e in profiled):.2f}-"
                f"{max(e['high_coverage'] for _, e in profiled):.2f} -> {owner}: "
                + ", ".join(f"#{i} {b}" for i, b, _ in touched))
            changes.extend(touched)
    changes += _neighbour_sweep(lines, voices, log)
    return changes


def _neighbour_sweep(lines, voices, log):
    """Give every still-unlabelled line the voice of the nearest labelled line.

    step11_refs_auto cannot leave a label without a reference, so "unknown" borrows
    whichever voice carries the most lines -- on this film that is the female lead, and
    every too-short-to-embed interjection ("Be honest.", "I know.") came out female. A
    line that is 0.4 s long has no timbre to measure, but it does sit inside somebody's
    turn, so its neighbour is the better guess.
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
