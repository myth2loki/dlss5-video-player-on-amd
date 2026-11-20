"""R3: reproduce the H.264 encode-throughput gate from plan section 2.3 Step 2.

Runs the same 1080p30 / 10s encode through every H.264 encoder the bundled
ffmpeg actually has, then applies the one hard threshold from section 11.6:
mean frame time below 20 ms. Prints a single PASS/FAIL judgement line so the
result cannot be read as "looks fine".

Usage:
    python3 tools/cast-poc/r3_encode_bench.py

Override ffmpeg directory with DLSS_BENCHMARK_FFMPEG (same convention as
tools/benchmark/common.py).
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FFMPEG_DIR = (
    Path(os.environ["DLSS_BENCHMARK_FFMPEG"])
    if os.environ.get("DLSS_BENCHMARK_FFMPEG")
    else REPO / "external" / "ffmpeg" / "bin"
)
FFMPEG = FFMPEG_DIR / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

THRESHOLD_MS = 20.0
DURATION_S = 10
WIDTH, HEIGHT, FPS = 1920, 1080, 30
# Hardware first: the plan's primary path is h264_amf on RX 9070 XT (gfx1201).
ARMS = ("h264_amf", "h264_nvenc", "libx264")


def available_encoders() -> set[str]:
    proc = subprocess.run(
        [str(FFMPEG), "-hide_banner", "-encoders"],
        capture_output=True, text=True, creationflags=FLAGS,
    )
    return set(re.findall(r"^\s*\S+\s+(\S+)\s", proc.stdout, re.M))


def encoder_flags(enc: str) -> list[str]:
    if enc == "h264_amf":
        return ["-quality", "speed", "-b:v", "8M"]
    if enc == "h264_nvenc":
        return ["-preset", "p1", "-b:v", "8M"]
    return ["-preset", "ultrafast", "-tune", "zerolatency", "-b:v", "8M"]


def parse_tail(text: str) -> dict | None:
    """Parse the last ffmpeg progress line. Elapsed may exceed a minute."""
    frames = re.findall(r"frame=\s*(\d+)", text)
    elapsed = re.findall(r"elapsed=(\d+):(\d+):([\d.]+)", text)
    speeds = re.findall(r"speed=\s*([\d.]+)x", text)
    if not frames or not elapsed:
        return None
    count = int(frames[-1])
    if count == 0:
        # Encoder present in the build but no usable device (e.g. h264_nvenc
        # on an AMD-only box). Unlike a slow encoder this is not an R3 failure.
        return None
    h, m, s = elapsed[-1]
    seconds = int(h) * 3600 + int(m) * 60 + float(s)
    speed = float(speeds[-1]) if speeds else None
    return {
        "frames": count,
        "elapsed_s": seconds,
        "speed": speed,
        # wall time per frame is the metric; speed is kept as a cross-check.
        "frame_ms": seconds * 1000.0 / count if count else float("inf"),
    }


def run_arm(enc: str) -> tuple[dict | None, str]:
    cmd = [
        str(FFMPEG), "-hide_banner", "-nostdin", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size={WIDTH}x{HEIGHT}:rate={FPS}",
        "-t", str(DURATION_S),
        "-c:v", enc, *encoder_flags(enc),
        "-f", "null", "-",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, creationflags=FLAGS, timeout=300)
    return parse_tail(proc.stdout + proc.stderr), (proc.stdout + proc.stderr)[-800:]


def main() -> int:
    if not FFMPEG.exists():
        print(f"POC R3 JUDGE  FAIL  reason=ffmpeg_missing path={FFMPEG}")
        return 2

    have = available_encoders()
    measured: dict[str, dict] = {}
    for enc in ARMS:
        if enc not in have:
            print(f"R3 ARM {enc}: SKIP reason=not_in_ffmpeg_encoders")
            continue
        result, tail = run_arm(enc)
        if result is None:
            print(f"R3 ARM {enc}: SKIP reason=no_progress_line tail={tail!r}")
            continue
        measured[enc] = result
        gate = "PASS" if result["frame_ms"] < THRESHOLD_MS else "FAIL"
        speed = f" speed={result['speed']:.2f}x" if result["speed"] is not None else ""
        print(
            f"R3 ARM {enc}: {gate} frames={result['frames']} "
            f"elapsed={result['elapsed_s']:.2f}s frame_ms={result['frame_ms']:.2f}"
            f"{speed} threshold_ms={THRESHOLD_MS:.0f}"
        )

    if "h264_amf" not in measured:
        print("POC R3 JUDGE  FAIL  reason=h264_amf_not_measured (target machine is gfx1201)")
        return 2

    amf = measured["h264_amf"]
    verdict = "PASS" if amf["frame_ms"] < THRESHOLD_MS else "FAIL"
    print(
        f"POC R3 JUDGE  {verdict}  encoder=h264_amf frame_ms={amf['frame_ms']:.2f} "
        f"threshold_ms={THRESHOLD_MS:.0f} frames={amf['frames']}"
    )
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
