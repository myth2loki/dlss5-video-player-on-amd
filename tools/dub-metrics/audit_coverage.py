"""How much of the film has a moving mouth and no Chinese coming out of it?

The fitter can squeeze a line shorter (tempo up to a ceiling) but it cannot lengthen one,
so when the translation is more compact than the English the Chinese clip stops early and
the rest of the shot plays in silence -- the original voice was removed by the separation.
Nothing measured that. The subtitle audit sees a cue that fits its own window; the mix
audit sees one line per source line; only the difference between where the speech is and
where the Chinese is shows the hole.

Both sides are measured on the same grid:

    speech   the source vocal stem, cut into runs the same way step1_align cuts them
             (per-line relative threshold, 0.35 s gap tolerance, film-wide floor)
    dub      [anchor, anchor + seconds] from mix_auto.json -- exactly what the mixer laid
             down, at exactly the level it laid it down

and the number reported is the speech that no dub audio overlaps.

Two guards keep the number honest, both of them lessons from this film:

  * a run also has to be audible -- within LEVEL dB of the film's median line level. The
    bridge scene at 258 s is a whisper the splitter left as -27 dBFS room tone; counting
    that as "someone is talking" would have blamed the dub for a hole that is really a
    quiet shot.
  * a run shorter than MIN_RUN is a breath or a click, not a mouth to fill.

The check is verified against the delivered file: for the worst stretches it compares the
master against the backing track at the same instants. If the master is within VERIFY dB of
the bed there, the hole is real -- what plays is music, not a voice.

Run:  DUB_JOB=... python tools/dub-metrics/audit_coverage.py
Exit code 1 when any line leaves MIN_HOLE or more of speech uncovered, or when the film as
a whole leaves more than MAX_SHARE.
"""
import json
import os
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import wavio                                   # noqa: E402

JOB = Path(os.environ.get("DUB_JOB", "F:/tmp/s2st/jobs/t5"))
WORK = JOB / "work"
VOCALS = Path(os.environ.get("DUB_VOCALS", JOB / "stems_out/vocals.wav"))
BED = Path(os.environ.get("DUB_BED", JOB / "stems_out/no_vocals.wav"))
MASTER = Path(os.environ.get("DUB_MASTER", JOB / "dubbed_full_zh2.wav"))

FRAME = 0.02          # the aligner's frame
GAP = 0.35            # a pause shorter than this is inside one line, not between two
RELAX = 0.12          # a run starts at 12% of the line's own peak
LEVEL = 10.0          # dB below the film's median line level: still speech, not room tone
MIN_RUN = 0.25        # shorter than this is a breath
MIN_HOLE = 0.25       # a hole worth reporting
MAX_SHARE = 0.01      # of all speech in the film
VERIFY = 2.0          # dB: master this close to the bed means nothing was mixed in


def energies(samples, rate, start, end):
    step = int(FRAME * rate)
    a, b = int(start * rate), int(end * rate)
    count = (b - a) // step
    if count <= 0:
        return np.zeros(0)
    return np.sqrt(np.mean(samples[a:a + count * step].reshape(count, step) ** 2, axis=1))


def speech_runs(samples, rate, start, end, floor):
    """Same rule as step1_align, so the metric and the aligner agree on what is speech."""
    levels = energies(samples, rate, start, end)
    if not len(levels) or float(levels.max()) < floor:
        return []
    threshold = max(float(levels.max()) * RELAX, floor * 0.6)
    voiced = levels >= threshold
    allowed = int(GAP / FRAME)
    runs, i = [], 0
    while i < len(voiced):
        if not voiced[i]:
            i += 1
            continue
        j = last = i
        while j < len(voiced):
            if voiced[j]:
                last = j
            elif j - last > allowed:
                break
            j += 1
        runs.append((start + i * FRAME, start + (last + 1) * FRAME))
        i = j
    return runs


def overlap(a, b, c, d):
    return max(0.0, min(b, d) - max(a, c))


def main():
    lines = {row["index"]: row for row in
             json.load(open(WORK / "lines.json", encoding="utf-8"))}
    try:
        mix = {row["index"]: row for row in
               json.load(open(WORK / "mix_auto.json", encoding="utf-8"))}
    except FileNotFoundError:
        mix = {}

    vocals, rate = wavio.read_wav(VOCALS)
    backing, _ = wavio.read_wav(BED)

    # The aligner's floor: the median of the per-line median energies, 12 dB down.
    positives = []
    for row in lines.values():
        levels = energies(vocals, rate, row.get("rough_start", row["start"]),
                          row.get("rough_end", row["end"]))
        if len(levels) and float(np.median(levels)) > 0:
            positives.append(float(np.median(levels)))
    floor = float(np.median(positives)) * 0.25
    quiet = float(np.median(positives)) * 10 ** (-LEVEL / 20)

    rows = []
    for index in sorted(lines):
        line = lines[index]
        layer = mix.get(index)
        runs = [r for r in speech_runs(vocals, rate, line["start"], line["end"], floor)
                if r[1] - r[0] >= MIN_RUN]
        audible = []
        for run in runs:
            level = float(np.sqrt(np.mean(
                vocals[int(run[0] * rate):int(run[1] * rate)] ** 2)))
            if level >= quiet:
                audible.append(run)
        speech = sum(b - a for a, b in audible)
        if layer is None:
            rows.append({"index": index, "speaker": line["speaker"], "start": line["start"],
                         "speech": speech, "covered": 0.0, "uncovered": speech,
                         "state": "no clip", "text": line.get("text", "")})
            continue
        a = layer["anchor"]
        b = a + layer["seconds"]
        covered = sum(overlap(x, y, a, b) for x, y in audible)
        first = min((x for x, _ in audible), default=a)
        last = max((y for _, y in audible), default=b)
        rows.append({
            "index": index, "speaker": line["speaker"], "start": line["start"],
            "speech": speech, "covered": covered, "uncovered": max(0.0, speech - covered),
            "late": a - first, "spill": b - last, "state": "ok",
            "text": layer.get("text", ""),
        })

    total_speech = sum(r["speech"] for r in rows)
    total_uncovered = sum(r["uncovered"] for r in rows)
    holes = [r for r in rows if r["uncovered"] >= MIN_HOLE]
    missing = [r for r in rows if r["state"] != "ok"]
    share = total_uncovered / max(total_speech, 1e-9)

    print(f"lines                 : {len(rows)}"
          + (f"   with no Chinese clip at all: {len(missing)}"
             if missing else ""))
    print(f"speech in the film    : {total_speech:6.1f} s   "
          f"(runs >= {MIN_RUN:.2f} s, at least {LEVEL:.0f} dB below the median line)")
    print(f"covered by Chinese    : {total_speech - total_uncovered:6.1f} s")
    print(f"left unheard          : {total_uncovered:6.1f} s   {share:.1%}"
          f"   (limit {MAX_SHARE:.0%})")
    print(f"lines with a hole >= {MIN_HOLE:.2f} s : {len(holes)}   (limit 0)")

    if holes:
        print("\nworst holes (speech with no Chinese over it):")
        for r in sorted(holes, key=lambda r: -r["uncovered"])[:10]:
            print(f"  #{r['index']:<3d} {r['speaker']:6s} {r['start']:7.2f}s  "
                  f"speech {r['speech']:4.2f}s  covered {r['covered']:4.2f}s  "
                  f"unheard {r['uncovered']:4.2f}s")
            if r["text"]:
                print(f"        {r['text'][:56]}")

    late = [r for r in rows if r["state"] == "ok" and r["late"] > MIN_HOLE]
    spill = [r for r in rows if r["state"] == "ok" and r["spill"] > MIN_HOLE]
    print(f"\nsync: {len(late)} line(s) start more than {MIN_HOLE:.2f} s after the "
          f"speech, {len(spill)} run past its end")
    if late:
        print("  late: " + ", ".join(
            f"#{r['index']} +{r['late']:.2f}s" for r in sorted(late, key=lambda r: -r["late"])[:6]))
    if spill:
        print("  past the end: " + ", ".join(
            f"#{r['index']} +{r['spill']:.2f}s"
            for r in sorted(spill, key=lambda r: -r["spill"])[:6]))

    # Confirm the worst stretches against the delivered file: if the master sits on top of
    # the backing track there, the audience is hearing music during a moving mouth.
    if holes and MASTER.exists():
        master, master_rate = wavio.read_wav(MASTER)
        checks = []
        for r in sorted(holes, key=lambda r: -r["uncovered"])[:5]:
            layer = mix[r["index"]]
            start = layer["anchor"] + layer["seconds"]
            end = start + min(r["uncovered"], 1.5)
            a, b = int(start * master_rate), int(end * master_rate)
            if b - a < master_rate // 10:
                continue
            here = float(np.sqrt(np.mean(master[a:b] ** 2)))
            bed = float(np.sqrt(np.mean(backing[a:b] ** 2))) if len(backing) > b else 0.0
            diff = 20 * np.log10(max(here, 1e-9) / max(bed, 1e-9))
            checks.append((r["index"], start, r["uncovered"], diff))
        if checks:
            print("\nverified against the master (delivered level over the bed, in the hole):")
            for index, start, length, diff in checks:
                verdict = "silent" if diff <= VERIFY else "has sound"
                print(f"  #{index:<3d} {start:7.2f}s +{length:4.2f}s  "
                      f"master is {diff:+.1f} dB over the bed   -> {verdict}")

    print(f"\ngate: {len(holes)} hole(s) (limit 0), {share:.1%} of speech unheard "
          f"(limit {MAX_SHARE:.0%})")
    return 1 if (holes or share > MAX_SHARE) else 0


if __name__ == "__main__":
    sys.exit(main())
