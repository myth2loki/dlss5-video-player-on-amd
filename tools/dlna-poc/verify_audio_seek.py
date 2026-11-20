"""POC verify item 2: independent audio decode child's input-seek lands on the
RIGHT absolute time (AudioPlayer.cpp:418 uses `-ss T -i file`, before -i).

Why this matters for DLNA casting: 11.3/14.4 opens a separate ffmpeg child that
decodes the current track from the player's Position() as the cast stream's
clock source. If the seek lands at 0 instead of T, the cast audio is a different
piece of content than the pictured frame - a silent, hard-to-notice mismatch.

Judgement (falsifiable):
  Build a 12 s track whose tone encodes absolute time: each 2 s slice has a
  distinct frequency 220/330/440/550/660/880 Hz. For each probe T in the MIDDLE
  of a slice, decode with the product's exact `-ss T -i` form and detect the
  dominant frequency of the first 0.4 s with a dependency-free Goertzel.
  PASS requires:
    (a) every probe's dominant frequency matches its slice (within tolerance);
    (b) control arm with NO -ss decodes 220 Hz - proving the detector can
        actually tell slices apart, so (a) is not vacuously true.

Usage:
    python3 tools/dlna-poc/verify_audio_seek.py
"""
from __future__ import annotations

import math
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FFMPEG_DIR = (
    Path(os.environ["DLSS_BENCHMARK_FFMPEG"])
    if os.environ.get("DLSS_BENCHMARK_FFMPEG")
    else REPO / "external" / "ffmpeg" / "bin"
)
FFMPEG = FFMPEG_DIR / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

RATE = 48000
SLICE_S = 2
FREQS = [220, 330, 440, 550, 660, 880]          # 12 s total, 2 s each
PROBES = [1.0, 5.0, 9.0]                         # middle of slices 0, 2, 4
WIN_S = 0.4
TOL_HZ = 18.0                                    # aac seek/encoder slack


def build_track(path: Path) -> None:
    """Concat six sine slices into one aac-in-mp4 track (product's real shape)."""
    ins: list[str] = []
    for f in FREQS:
        ins += ["-f", "lavfi", "-i", f"sine=frequency={f}:sample_rate={RATE}:duration={SLICE_S}"]
    graph = "".join(f"[{i}:a]" for i in range(len(FREQS))) + f"concat=n={len(FREQS)}:v=0:a=1[out]"
    cmd = [str(FFMPEG), "-hide_banner", "-nostdin", "-y", *ins,
           "-filter_complex", graph, "-map", "[out]", "-c:a", "aac", "-b:a", "128k", str(path)]
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=FLAGS, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"track build failed: {p.stderr[-600:]}")


def decode_pcm(path: Path, seek: float | None) -> bytes:
    """Decode first WIN_S seconds exactly as AudioPlayer does (input -ss)."""
    cmd = [str(FFMPEG), "-hide_banner", "-nostdin", "-loglevel", "error"]
    if seek is not None:
        cmd += ["-ss", f"{seek:.6f}"]
    cmd += ["-i", str(path), "-map", "0:a:0", "-vn", "-ac", "1", "-ar", str(RATE),
            "-t", f"{WIN_S:.3f}", "-f", "s16le", "-c:a", "pcm_s16le", "pipe:1"]
    p = subprocess.run(cmd, capture_output=True, creationflags=FLAGS, timeout=60)
    if p.returncode != 0:
        raise RuntimeError(f"decode failed: {p.stderr[-400:]!r}")
    return p.stdout


def dominant_freq(pcm: bytes, candidates: list[int]) -> int:
    """Goertzel on raw s16le mono; return the candidate with most energy."""
    n = len(pcm) // 2
    if n < RATE // 10:
        raise RuntimeError(f"too few samples: {n}")
    x = struct.unpack(f"<{n}h", pcm[: n * 2])
    best, best_pow = candidates[0], -1.0
    for f in candidates:
        k = 2.0 * math.cos(2.0 * math.pi * f / RATE)
        s1 = s2 = 0.0
        for v in x:
            s0 = v + k * s1 - s2
            s2, s1 = s1, s0
        power = s1 * s1 + s2 * s2 - k * s1 * s2
        if power > best_pow:
            best, best_pow = f, power
    return best


def expected_freq(t: float) -> int:
    return FREQS[min(int(t // SLICE_S), len(FREQS) - 1)]


def main() -> int:
    if not FFMPEG.exists():
        print(f"POC SEEK JUDGE  FAIL  reason=ffmpeg_missing path={FFMPEG}")
        return 2
    with tempfile.TemporaryDirectory(prefix="seek-poc-") as td:
        track = Path(td) / "seek_test.mp4"
        try:
            build_track(track)
        except RuntimeError as e:
            print(f"POC SEEK JUDGE  FAIL  reason=track_build {e}")
            return 2

        ok = True
        for t in PROBES:
            want = expected_freq(t)
            got = dominant_freq(decode_pcm(track, t), FREQS)
            hit = got == want
            ok &= hit
            print(f"POC SEEK PROBE t={t:.1f}s want={want}Hz got={got}Hz {'PASS' if hit else 'FAIL'}")
        # Control: without -ss the first slice must be 220 Hz; otherwise the
        # detector has no discriminating power and the probes above prove nothing.
        ctrl = dominant_freq(decode_pcm(track, None), FREQS)
        ctrl_ok = ctrl == FREQS[0]
        ok &= ctrl_ok
        print(f"POC SEEK CONTROL no-ss want={FREQS[0]}Hz got={ctrl}Hz {'PASS' if ctrl_ok else 'FAIL'}")

    verdict = "PASS" if ok else "FAIL"
    print(f"POC SEEK JUDGE  {verdict}  input-seek T matches absolute content; "
          f"tolerance={TOL_HZ:.0f}Hz probes={len(PROBES)}+1control")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
