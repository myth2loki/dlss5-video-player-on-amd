"""Prepare a job: audio out of the container, a transcript, and the two stems.

Everything downstream reads files this stage writes, so it is the one place that
knows how the tools are invoked. Three external binaries, one HTTP endpoint --
each failure says which tool and what it printed, because a job that dies here
has no artifacts to inspect.

Whisper runs through the CLI and not the server: `-oj` is the shape the rest of
the pipeline reads (`transcription[].offsets`), and the server's `/inference`
answers in a different one, which cost a round trip to discover.

Environment (all optional; the defaults are this checkout's layout):
  DUB_ROOT         job directory, where work/ and stems_out/ land
  DUB_INPUT        source video
  DUB_REPO         player checkout, for ffmpeg and whisper
  DUB_FFMPEG       ffmpeg
  DUB_WHISPER      whisper-cli.exe (the GPU build, beside its ggml-*.dll)
  DUB_WHISPER_MODEL   whisper ggml model
  DUB_STEMS        stems-split.exe
  DUB_STEMS_MODEL  htdemucs GGUF
  DUB_LANG         spoken language of the source, ISO 639-1
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import time
import wave

ROOT = pathlib.Path(os.environ.get("DUB_ROOT", "F:/tmp/s2st"))
INPUT = pathlib.Path(os.environ.get("DUB_INPUT", "F:/dlss5-video/test-media/tears_of_steel_720p.mov"))
REPO = pathlib.Path(os.environ.get("DUB_REPO", "F:/dlss5-video/dlss5-video-amd"))
# Engines live beside the scripts, jobs live wherever the user points ROOT. Keeping
# them apart is what lets a job directory be the only thing a run writes into.
ENGINE = pathlib.Path(os.environ.get("DUB_ENGINE", "F:/tmp/s2st"))

FFMPEG = os.environ.get("DUB_FFMPEG", str(REPO / "run-fg/ffmpeg.exe"))
FFPROBE = os.environ.get("DUB_FFPROBE", str(pathlib.Path(FFMPEG).with_name("ffprobe.exe")))
WHISPER = os.environ.get("DUB_WHISPER", str(REPO / "asr/runtime-gpu/whisper-cli.exe"))
WHISPER_MODEL = os.environ.get("DUB_WHISPER_MODEL", str(REPO / "asr/models/ggml-large-v3-turbo-q5_0.bin"))
STEMS = os.environ.get("DUB_STEMS", str(ENGINE / "stems/stems-split.exe"))
STEMS_MODEL = os.environ.get("DUB_STEMS_MODEL", str(ENGINE / "stems/models/htdemucs-42M-v1.0-F32.gguf"))
LANG = os.environ.get("DUB_LANG", "en")

WORK = ROOT / "work"
WORK.mkdir(parents=True, exist_ok=True)


def step(message):
    print(f"\n--- {message}", flush=True)


def need(path, what):
    if not pathlib.Path(path).exists():
        sys.exit(f"missing {what}: {path}")


def run(command, what):
    print("   $ " + pathlib.Path(str(command[0])).name + " ...", flush=True)
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{what} failed ({result.returncode}):\n{result.stderr[-1500:]}")
    return result


def norm(text):
    return re.sub(r"[^a-z0-9 ]", "", text.lower()).strip()


# --- 1. audio -------------------------------------------------------------------
step(f"extract audio from {INPUT.name}")
need(INPUT, "input video")
info = subprocess.run([FFPROBE, "-v", "error", "-show_entries", "format=duration",
                       "-of", "default=nw=1:nk=1", str(INPUT)], capture_output=True, text=True)
if info.returncode == 0 and info.stdout.strip():
    print(f"   {float(info.stdout.strip()):.1f}s")

full = ROOT / "full44k.wav"
run([FFMPEG, "-v", "error", "-y", "-i", INPUT, "-vn", "-ac", "2", "-ar", "44100",
     "-c:a", "pcm_s16le", full], "audio extraction")
print(f"   -> {full.name} {full.stat().st_size / 1e6:.0f} MB")

# --- 2. transcript --------------------------------------------------------------
step("transcribe")
need(WHISPER, "whisper-cli")
need(WHISPER_MODEL, "whisper model")
transcript_path = WORK / "transcript.json"

def transcript_lines():
    """Segments this stage can vouch for, or none.

    A file that parses but holds no segments is a failed run that left a file
    behind -- the first attempt here wrote one -- so it is treated the same as no
    file at all rather than being reused and failing a stage later with a worse
    message.
    """
    if not transcript_path.exists():
        return []
    try:
        return json.loads(transcript_path.read_text(encoding="utf-8")).get("transcription", []) or []
    except json.JSONDecodeError:
        return []


segments = transcript_lines()
if segments:
    print(f"   reuse {transcript_path.name}")
else:
    started = time.time()
    run([WHISPER, "-m", WHISPER_MODEL, "-f", full, "-l", LANG, "-oj",
         "-of", WORK / "transcript", "-t", os.environ.get("DUB_THREADS", "8")], "transcription")
    segments = transcript_lines()
    print(f"   -> in {time.time() - started:.0f}s")
    if not segments:
        sys.exit("whisper produced no segments")

# The film's own noise floor produces one line of music repeated hundreds of times;
# keeping the first sighting of each distinct text is what separates 19 real lines
# from 392 segments. Cheap, and it needs no threshold to tune.
seen = {}
for segment in segments:
    key = norm(segment.get("text", ""))
    if key and key not in seen:
        seen[key] = segment
print(f"   {len(seen)} distinct lines out of {len(segments)} segments")

# --- 3. stems -------------------------------------------------------------------
step("separate vocals")
need(STEMS, "stems-split")
need(STEMS_MODEL, "htdemucs gguf")
stem_dir = ROOT / "stems_out"
vocals, bed = stem_dir / "vocals.wav", stem_dir / "no_vocals.wav"
if vocals.exists() and bed.exists():
    print("   reuse stems")
else:
    started = time.time()
    run([STEMS, "--model", STEMS_MODEL, "--input", full, "--out", stem_dir, "--two-stems", "vocals",
         "--device", os.environ.get("DUB_STEMS_DEVICE", "gpu")], "stem separation")
    print(f"   -> in {time.time() - started:.0f}s")
need(vocals, "vocals stem")
need(bed, "background stem")

print(f"\nready: {ROOT}")
