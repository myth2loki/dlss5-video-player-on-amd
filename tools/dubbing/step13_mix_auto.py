"""Lay the dubbed lines onto the separated backing track and render the deliverables.

Different from step4: timing was already solved in step12, so this stage does no
stretching. It places each clip at the anchor that step12 computed, which may sit up to
0.8 s before the line's own start when the Chinese needed more room than the silence
after the line provided.

Overlap is checked rather than assumed away: if a clip still runs into the next line's
anchor, that is reported, because two voices talking over each other is audible even
when every individual line meets its own window.
"""

import json
import os
import pathlib
import subprocess

import numpy as np
import soundfile as sf

ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
WORK = ROOT / "work"
DUB = ROOT / "dub_auto"
FFMPEG = os.environ.get("DUB_FFMPEG", str(REPO / "run-fg/ffmpeg.exe"))
BED = os.environ.get("DUB_BED", str(ROOT / "stems_out/no_vocals.wav"))
VOCALS = os.environ.get("DUB_VOCALS", str(ROOT / "stems_out/vocals.wav"))
RATE = 44100
GAIN_BACKING = 0.85
HARD_TEMPO = 1.35     # ceiling when rushing a line is the only way to avoid a collision
MIN_GAP = 0.03        # silence left between two lines so they do not butt together


def stretch(path, ratio, temp):
    """Time-stretch a clip without moving its pitch, and hand back the samples."""
    subprocess.run([FFMPEG, "-v", "error", "-y", "-i", str(path),
                    "-filter:a", f"atempo={ratio:.3f}", str(temp)], check=True)
    audio, rate = sf.read(str(temp), always_2d=True)
    return resample(audio.mean(axis=1), rate)


def resample(samples, rate):
    """Any input rate -> RATE, through ffmpeg's soxr resampler.

    This used to call librosa, which dragged numba + llvmlite + scipy into a stage whose
    only other needs are numpy and soundfile. ffmpeg is already a hard requirement here
    (it does the atempo and the mux), and librosa resamples with soxr as well -- at
    precision 28 the output is bit-identical to librosa's, so nothing about the mix
    changed when the dependency went away.
    """
    if rate == RATE:
        return np.asarray(samples, dtype=np.float32)
    src = np.asarray(samples, dtype=np.float32).tobytes()
    done = subprocess.run(
        [FFMPEG, "-v", "error", "-f", "f32le", "-ar", str(rate), "-ac", "1", "-i", "pipe:0",
         "-resampler", "soxr", "-precision", "28",
         "-f", "f32le", "-ar", str(RATE), "-ac", "1", "pipe:1"],
        input=src, capture_output=True, check=True)
    return np.frombuffer(done.stdout, dtype=np.float32)


def resample_any(samples, rate):
    """Same, for a clip that may be multichannel -- channels are resampled independently."""
    samples = np.asarray(samples, dtype=np.float32)
    if samples.ndim == 1:
        return resample(samples, rate)
    return np.stack([resample(samples[:, c], rate) for c in range(samples.shape[1])], axis=1)


def main():
    rows = json.load(open(WORK / "fit_auto.json", encoding="utf-8"))
    lines = {row["index"]: row for row in json.load(open(WORK / "lines.json", encoding="utf-8"))}

    # The backing track keeps its channels. Collapsing it to mono to make the arithmetic
    # easy is audible on headphones -- every music cue loses its width -- and the dub line
    # is mono anyway, so it just goes into both channels.
    #
    # GAIN_BACKING was tuned when the bed was the mono sum, i.e. each channel sat at the
    # level of (L+R)/2, which for a partly decorrelated mix is 1.5-2 dB quieter than either
    # channel alone. Scaling by that ratio keeps the voice-to-music balance the mix was
    # accepted with, instead of letting a louder bed duck the lines.
    backing, b_rate = sf.read(BED, always_2d=True)
    mono_rms = float(np.sqrt(np.mean(backing.mean(axis=1) ** 2)))
    channel_rms = float(np.sqrt(np.mean(backing ** 2)))
    bed_gain = GAIN_BACKING * mono_rms / (channel_rms + 1e-12)

    # Duck the bed only where a Chinese line actually plays. Holding it at bed_gain for
    # the whole film put every pause 5.5 dB under the level the mix was made at, and a
    # pause with score under it then reads as the audio having dropped out -- the
    # close-up at 5:06-5:16, where nobody speaks in the original either, was reported as
    # "no voice here" for exactly that reason. Between lines the bed now stays as loud as
    # the film shipped, and only the lines pull it down.
    sound = resample_any(backing, b_rate)
    envelope = np.ones(len(sound), dtype=np.float32)
    for row in rows:
        a = max(0, int((row["anchor"] - 0.20) * RATE))
        b = min(len(envelope), int((row["anchor"] + row["seconds"] + 0.20) * RATE))
        if b - a > 1:
            envelope[a:b] = np.minimum(envelope[a:b], bed_gain)
    # 150 ms ramps at every step, or the duck clicks. A convolution over 32 M samples is
    # far too slow for a fixed two-level curve, so the ramps are drawn at the steps.
    ramp = int(0.15 * RATE)
    for edge in np.flatnonzero(np.abs(np.diff(envelope)) > 1e-6):
        lo = max(0, edge - ramp)
        hi = min(len(envelope), edge + ramp + 2)
        envelope[lo:hi] = np.linspace(envelope[lo], envelope[hi - 1], hi - lo)
    scene = sound * envelope[:, None]
    print(f"backing {len(scene)/RATE:.1f}s, {scene.shape[1]} ch, "
          f"bed {20*np.log10(mono_rms/channel_rms):+.1f} dB vs mono sum, "
          f"ducked {20*np.log10(bed_gain):.1f} dB under {len(rows)} line(s)")

    order = sorted(rows, key=lambda r: r["anchor"])
    placed = []
    overlaps = []
    hurried = []
    trimmed = []

    # Loudness. Qwen3-TTS returns a different level for every line -- across one film the
    # clips ranged 0.010 to 0.11 RMS, so the quietest sat 20 dB under the rest and simply
    # vanished under the music ("this line has no sound"). Each clip is brought to the
    # median level of the set, with the correction capped at +-12 dB so a line that is
    # genuinely whispered stays whispered.
    levels = {}
    for row in rows:
        clip = DUB / f"zh{row['index']:02d}.wav"
        if clip.exists():
            data, _ = sf.read(str(clip), always_2d=True)
            data = data.mean(axis=1)
            levels[row["index"]] = float(np.sqrt(np.mean(data ** 2))) if len(data) else 0.0
    target = float(np.median(list(levels.values()))) if levels else 0.0
    if levels:
        print(f"clip level: median {target:.3f}, "
              f"range {min(levels.values()):.3f}-{max(levels.values()):.3f}")

    for position, row in enumerate(order):
        index = row["index"]
        path = DUB / f"zh{index:02d}.wav"
        if not path.exists():
            print(f"  #{index:2d} missing clip")
            continue
        audio, rate = sf.read(str(path), always_2d=True)
        audio = resample(audio.mean(axis=1), rate)
        seconds = len(audio) / RATE

        if position and placed:
            previous = placed[-1]
            previous_end = previous["anchor"] + previous["seconds"]
            if row["anchor"] < previous_end - 0.01:
                overlaps.append({"index": index, "previous": previous["index"],
                                 "amount": round(previous_end - row["anchor"], 2)})

        # A line that runs into the next one puts two voices in the room at once, which
        # is the one defect a listener cannot hear past. Rushing this line further is the
        # cheaper loss: the fitting stage already stopped at 1.2x, and up to 1.35x a short
        # line still reads. Past that the tail goes, with the fade that is already applied.
        limit = None
        if position + 1 < len(order):
            limit = order[position + 1]["anchor"] - row["anchor"] - MIN_GAP
        if limit is not None and limit > 0.25 and seconds > limit:
            ratio = min(seconds / limit, HARD_TEMPO)
            audio = stretch(path, ratio, DUB / f"zh{index:02d}_p.wav")
            seconds = len(audio) / RATE
            hurried.append((index, round(ratio, 2)))
            if seconds > limit:
                audio = audio[:int(limit * RATE)]
                seconds = len(audio) / RATE
                trimmed.append(index)

        fade = min(int(0.03 * RATE), len(audio) // 4)
        if fade > 0:
            audio = audio.copy()
            audio[:fade] *= np.linspace(0, 1, fade)
            audio[-fade:] *= np.linspace(1, 0, fade)

        gain = 1.0
        if target and levels.get(index):
            gain = min(max(target / levels[index], 0.25), 4.0)   # +-12 dB
            audio = audio * gain

        begin = int(row["anchor"] * RATE)
        end = min(begin + len(audio), len(scene))
        if end > begin:
            # mono line -> every channel, same level
            scene[begin:end] += audio[:end - begin][:, None]
        placed.append({"index": index, "anchor": row["anchor"], "seconds": round(seconds, 2),
                       "gain": round(gain, 2), "speaker": row["speaker"], "text": row["text"]})

    # Nothing the actor said may end up silent. The bed has every English voice removed,
    # so a line that never got a Chinese clip -- translation rejected, or the text turned
    # out to sit where nobody is speaking -- leaves a hole where a mouth is still moving.
    # Those lines get their own original voice back, and only those.
    #
    # Deliberately per line rather than by scanning the vocal stem for uncovered speech:
    # the spans we hold are tighter than the utterances (they cover the loudest run, not
    # every syllable), so a scan reports the tail of every line as "uncovered" and dubs
    # the English back underneath the Chinese. Measured, that put 60 s of English into a
    # 12-minute Chinese track.
    restored = []
    dubbed = {item["index"] for item in placed}
    if pathlib.Path(VOCALS).exists():
        vocals, v_rate = sf.read(VOCALS, always_2d=True)
        vox_clip = resample_any(vocals, v_rate)
        for row in lines.values():
            if row["index"] in dubbed:
                continue
            a = max(0, int((row["start"] - 0.25) * RATE))
            b = min(len(scene), int((row["end"] + 0.25) * RATE))
            if b - a < int(0.2 * RATE):
                continue
            piece = vox_clip[a:b].copy()
            ramp = min(int(0.02 * RATE), len(piece) // 2)
            if ramp > 0:
                piece[:ramp] *= np.linspace(0, 1, ramp)[:, None]
                piece[-ramp:] *= np.linspace(1, 0, ramp)[:, None]
            scene[a:b] += piece * 0.9
            restored.append((row["index"], round(row["start"], 2)))
    if restored:
        print(f"  original voice restored for {len(restored)} line(s) that never got a "
              f"Chinese clip: {restored}")

    peak = float(np.abs(scene).max()) or 1.0
    if peak > 0.95:
        scene = scene / peak * 0.95

    mono = ROOT / "dubbed_full_zh2.wav"
    sf.write(str(mono), scene.astype(np.float32), RATE)
    subprocess.run([FFMPEG, "-v", "error", "-y", "-i", str(mono),
                    "-c:a", "libmp3lame", "-b:a", "192k",
                    str(ROOT / "dubbed_full_zh2.mp3")], check=True)
    print(f"wrote {mono.name}  ({len(scene)/RATE:.1f}s, peak {float(np.abs(scene).max()):.2f})")

    # Line-by-line A/B: original English, then the Chinese that replaced it.
    original, original_rate = sf.read(str(ROOT / "full44k.wav"), always_2d=True)
    channels = original.shape[1]
    silent = lambda seconds: np.zeros((int(seconds * RATE), channels), dtype=np.float32)
    pieces = []
    for row in sorted(rows, key=lambda r: r["index"]):
        index = row["index"]
        line = lines[index]
        english = original[int(line["start"] * original_rate):int(line["end"] * original_rate)]
        pieces += [english.astype(np.float32), silent(0.35)]
        audio, rate = sf.read(str(DUB / f"zh{index:02d}.wav"), always_2d=True)
        zh = resample(audio.mean(axis=1), rate)
        pieces += [np.repeat(zh[:, None], channels, axis=1), silent(0.7)]
    montage = np.concatenate(pieces)
    peak = float(np.abs(montage).max()) or 1.0
    if peak > 0.95:
        montage = montage / peak * 0.95
    sf.write(str(ROOT / "ab_lines_zh2.wav"), montage.astype(np.float32), RATE)
    subprocess.run([FFMPEG, "-v", "error", "-y", "-i", str(ROOT / "ab_lines_zh2.wav"),
                    "-c:a", "libmp3lame", "-b:a", "192k",
                    str(ROOT / "ab_lines_zh2.mp3")], check=True)
    print(f"wrote ab_lines_zh2.mp3 ({len(montage)/RATE:.1f}s)")

    # Muxing the picture is the driver's job, not this stage's: the source video is
    # whatever the caller was given, and this stage has no business knowing its path.
    print(f"\n=== {len(placed)} 句铺放 ===")
    if overlaps:
        print("重叠: " + ", ".join(f"#{o['index']}<-{o['previous']} {o['amount']}s" for o in overlaps))
    else:
        print("无重叠")
    if hurried:
        print("为避免重叠再加速: " + ", ".join(f"#{i}(x{r})" for i, r in hurried))
    if trimmed:
        print("仍放不下、掐尾: " + ", ".join(f"#{i}" for i in trimmed))
    shifted = [r for r in rows if r.get("shift", 0) > 0.01]
    print(f"起点前移 {len(shifted)} 句: " + ", ".join(
        f"#{r['index']}(-{r['shift']}s)" for r in shifted) if shifted else "起点前移 0 句")
    stretched = [r for r in rows if r.get("tempo", 1.0) > 1.02]
    print(f"atempo {len(stretched)} 句: " + ", ".join(
        f"#{r['index']}(x{r['tempo']})" for r in stretched) if stretched else "atempo 0 句")
    json.dump(placed, open(WORK / "mix_auto.json", "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
