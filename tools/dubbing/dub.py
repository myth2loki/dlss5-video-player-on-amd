"""Dub a video into another language, end to end.

    python dub.py movie.mov
    python dub.py movie.mov --out D:/jobs/movie --to zh

Six stages, each a separate process writing files the next one reads, so a run
that goes wrong can be restarted from any stage with --from and leaves its
intermediates behind with --keep:

    0  audio, transcript, stems        step0_prepare.py
    1  line timing + who is speaking   step1_align.py
    2  reference audio per voice       step11_refs_auto.py
    3  draft translation               step2t_translate.py
    4  fit to the original timing, synth  step12_fit_auto.py
    5  lay onto the backing track      step13_mix_auto.py
       mux video + dubbed audio

The pace model is a property of the voice engine, not of the film, so it is read
from the engine directory and copied into each job rather than measured per run.
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ENGINE = pathlib.Path(os.environ.get("DUB_ENGINE", str(HERE)))
REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
FFMPEG = os.environ.get("DUB_FFMPEG", str(REPO / "run-fg/ffmpeg.exe"))

STAGES = [
    ("prepare", "step0_prepare.py"),
    ("align", "step1_align.py"),
    ("refs", "step11_refs_auto.py"),
    ("translate", "step2t_translate.py"),
    ("fit", "step12_fit_auto.py"),
    ("mix", "step13_mix_auto.py"),
]


def banner(text):
    print(f"\n{'=' * 62}\n{text}\n{'=' * 62}", flush=True)


def run_stage(script, env):
    started = time.time()
    result = subprocess.run([sys.executable, str(HERE / script)], env=env)
    if result.returncode != 0:
        sys.exit(f"\n{script} failed with {result.returncode}")
    return time.time() - started


def main():
    parser = argparse.ArgumentParser(description="Dub a video into Chinese speech.")
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, help="job directory")
    parser.add_argument("--output", type=pathlib.Path, help="finished video path")
    parser.add_argument("--lang", default="en", help="spoken language in the source")
    parser.add_argument("--from", dest="start", default="prepare",
                        help="start at this stage: " + ", ".join(name for name, _ in STAGES))
    parser.add_argument("--stop-after", dest="stop", default="mix",
                        help="stop after this stage")
    parser.add_argument("--keep", action="store_true", help="keep intermediate audio")
    args = parser.parse_args()

    source = args.input.resolve()
    if not source.exists():
        sys.exit(f"no such file: {source}")
    job = (args.out or HERE / "jobs" / source.stem).resolve()
    job.mkdir(parents=True, exist_ok=True)
    (job / "work").mkdir(exist_ok=True)

    # The engine's pace model travels with the job: stage 4 reads it from work/,
    # and a fresh job has no reason to measure the same voice again.
    pace = ENGINE / "pace_model.json"
    if pace.exists() and not (job / "work/pace_model.json").exists():
        shutil.copy2(pace, job / "work/pace_model.json")

    env = dict(os.environ)
    env.update({
        "DUB_ROOT": str(job),
        "DUB_INPUT": str(source),
        "DUB_ENGINE": str(ENGINE),
        "DUB_REPO": str(REPO),
        "DUB_FFMPEG": FFMPEG,
        "DUB_LANG": args.lang,
        "DUB_VOCALS": str(job / "stems_out/vocals.wav"),
        "DUB_BED": str(job / "stems_out/no_vocals.wav"),
        "DUB_TRANSCRIPT": str(job / "work/transcript.json"),
        # One glossary for the translator and the fitter, so a name cannot be right
        # in the ear and wrong on screen.
        "DUB_GLOSSARY": str(ENGINE / "glossary.json"),
        "PYTHONIOENCODING": "utf-8",
    })

    banner(f"{source.name}  ->  {job}")
    started_at = time.time()
    timings = []
    names = [name for name, _ in STAGES]
    begin = names.index(args.start)
    end = names.index(args.stop)
    for name, script in STAGES[begin:end + 1]:
        banner(f"[{name}] {script}")
        timings.append((name, run_stage(script, env)))

    dubbed = job / "dubbed_full_zh2.wav"
    if not dubbed.exists():
        if args.stop != STAGES[-1][0]:
            # Asked to stop early on purpose: there is no mix to mux yet, and that is fine.
            banner("stopped")
            for name, seconds in timings:
                print(f"  {name:10s} {seconds:6.1f}s")
            return
        sys.exit(f"expected {dubbed} from the mix stage")
    target = (args.output or job / f"{source.stem}_zh{source.suffix}").resolve()
    banner(f"[mux] {target.name}")
    # The subtitle file the fit stage wrote from the same rows it spoke. It has to be
    # mapped explicitly: leaving it out is what produced a dubbed film where every
    # line of Chinese existed on disk and none of it was on screen.
    subtitle_file = job / "subtitles.ass"
    inputs = ["-i", str(source), "-i", str(dubbed)]
    maps = ["-map", "0:v:0", "-map", "1:a:0"]
    # 256k, not 128k or 192k: the master is limited to -2 dBTP, and the AAC decoder puts
    # some of it back. Measured on this film, decoded samples above full scale: 249 in the
    # source's own MP3, 12 at 128k, 2 at 256k. The peak value does not fall with bitrate
    # (a 0.03 dB change in the master moves it by 1.7 dB), so the count is the honest
    # measure -- two samples in twelve minutes is 45 us of clipping.
    #
    # The picture is copied, not re-encoded. DUB_VIDEO_CRF overrides that when the file has
    # to be small enough to hand to someone -- crf 30 turns this film into 86 MB, which is
    # what the delivered version used.
    crf = os.environ.get("DUB_VIDEO_CRF")
    picture = (["-c:v", "libx264", "-preset", "medium", "-crf", crf] if crf
               else ["-c:v", "copy"])
    codecs = picture + ["-c:a", "aac", "-b:a", "256k"]
    if subtitle_file.exists():
        inputs += ["-i", str(subtitle_file)]
        maps += ["-map", "2:s:0"]
        if target.suffix.lower() in (".mkv", ".mk3d"):
            codecs += ["-c:s", "ass"]
        else:
            # mp4 carries mov_text, not ASS: the glyph styling is dropped but the
            # words and their timing survive, and the viewer can turn them off.
            codecs += ["-c:s", "mov_text"]
        # `chi`, not `zho`: the mov muxer writes the ISO 639-2/B (bibliographic)
        # code, and silently drops a T-code tag, leaving the track with no
        # language at all.
        codecs += ["-metadata:s:s:0", "language=chi", "-disposition:s:0", "0"]
    subprocess.run([FFMPEG, "-v", "error", "-y", *inputs, *maps, *codecs,
                    "-movflags", "+faststart", str(target)], check=True)

    rows = json.load(open(job / "work/fit_auto.json", encoding="utf-8"))
    over = [row for row in rows if row.get("needs_human")]
    banner("done")
    for name, seconds in timings:
        print(f"  {name:10s} {seconds:6.1f}s")
    print(f"  {'total':10s} {time.time() - started_at:6.1f}s")
    print(f"  {len(rows)} lines, {len(over)} still over their window"
          + (": " + ", ".join(f"#{row['index']}" for row in over) if over else ""))
    print(f"  {target}  ({target.stat().st_size / 1e6:.0f} MB)")

    if not args.keep:
        for scratch in ("full44k.wav", "ab_lines_zh2.wav"):
            (job / scratch).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
