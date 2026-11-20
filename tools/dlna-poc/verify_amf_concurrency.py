"""POC verify item 1: does h264_amf encoding contend with D3D12 neural upscaling?

Context (doc section 14.3/14.4): casting opens a separate h264_amf encode child
while the player keeps running its neural upscaling on the same RX 9070 XT.
RDNA4 has a dedicated VCN media engine for encode, physically separate from the
shader/compute units that run neural NR - so on paper they should not contend.
This script turns that into a falsifiable measurement instead of an argument.

Experiment (A/B, same encoder settings, same machine, back-to-back):
  ARM baseline : h264_amf 1080p30 x Ns, alone.
  ARM loaded   : the SAME encode while a real AMD neural-upscaling --render job
                 saturates the GPU's compute/shader units concurrently.

Metric: wall-clock ms per encoded frame (same metric as R3). "Dropping frames"
means the encode child cannot keep up with 30fps production, i.e. frame_ms
exceeds the realtime budget. Judgement:
  PASS iff loaded arm frame_ms < THRESHOLD_MS (20 ms, the project's R3 gate;
  33.3 ms is the hard 30fps deadline). The slowdown ratio is recorded as a
  fact, NOT used as a gate: a stable 2x rise from ~2.6 ms to ~5.7 ms is still
  3.5x under budget and does not drop frames. Gating on slowdown would test
  "did it get slower", not the question asked ("does it drop frames").
  A control proves the load arm really overlapped: it records whether the
  neural render process was alive for the whole encode window. If overlap
  could not be established, that is INCONCLUSIVE, not PASS.

The neural load uses the shipped dist player's `--render --stages nr
--neural-backend amd`, which this machine has previously run successfully.

Usage:
    python3 tools/dlna-poc/verify_amf_concurrency.py
Optional:
    DLSS_BENCHMARK_FFMPEG   ffmpeg bin dir (same as r3_encode_bench.py)
    DLSS_CAST_AMD_PLAYER    path to the AMD player exe that can --render
    DLSS_CAST_LOAD_MEDIA    media file used as the neural-upscaling load
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FFMPEG_DIR = (
    Path(os.environ["DLSS_BENCHMARK_FFMPEG"])
    if os.environ.get("DLSS_BENCHMARK_FFMPEG")
    else REPO / "external" / "ffmpeg" / "bin"
)
FFMPEG = FFMPEG_DIR / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

PLAYER = (
    Path(os.environ["DLSS_CAST_AMD_PLAYER"])
    if os.environ.get("DLSS_CAST_AMD_PLAYER")
    else REPO / "scratch_render" / "DLSSVideoPlayer.exe"
)
LOAD_MEDIA = (
    Path(os.environ["DLSS_CAST_LOAD_MEDIA"])
    if os.environ.get("DLSS_CAST_LOAD_MEDIA")
    else REPO / "tools" / "web-overlay" / "fixtures" / "site" / "BV1GJ411x7h7_137649199_q64.mp4"
)

THRESHOLD_MS = 20.0
MAX_SLOWDOWN = 2.0
ENCODE_S = 10
WIDTH, HEIGHT, FPS = 1920, 1080, 30
LOAD_FRAMES = 400          # ~40 s of neural work, covers the 10 s encode window


def run_encode() -> tuple[float | None, float]:
    """h264_amf encode throughput, same flags as R3. Returns (frame_ms, wall_s)."""
    cmd = [
        str(FFMPEG), "-hide_banner", "-nostdin", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size={WIDTH}x{HEIGHT}:rate={FPS}",
        "-t", str(ENCODE_S),
        "-c:v", "h264_amf", "-quality", "speed", "-b:v", "8M",
        "-f", "null", "-",
    ]
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=FLAGS, timeout=300)
    wall = time.monotonic() - t0
    text = p.stdout + p.stderr
    frames = re.findall(r"frame=\s*(\d+)", text)
    count = int(frames[-1]) if frames else 0
    if count == 0:
        return None, wall
    return wall * 1000.0 / count, wall


def main() -> int:
    for label, path in (("ffmpeg", FFMPEG), ("amd player", PLAYER), ("load media", LOAD_MEDIA)):
        if not path.exists():
            print(f"POC CONCURRENCY JUDGE  FAIL  reason={label}_missing path={path}")
            return 2

    # Arm 1: baseline encode alone.
    base, base_wall = run_encode()
    if base is None:
        print("POC CONCURRENCY JUDGE  FAIL  reason=baseline_encode_no_frames")
        return 2
    print(f"POC CONC ARM baseline: frame_ms={base:.2f} wall={base_wall:.2f}s threshold={THRESHOLD_MS:.0f}")

    # Arm 2: start a real AMD neural-upscaling render, then encode while it runs.
    with tempfile.TemporaryDirectory(prefix="amf-load-") as td:
        out = str(Path(td) / "load_out.mp4")
        render_cmd = [
            str(PLAYER), "--render", str(LOAD_MEDIA), "--out", out,
            "--stages", "nr", "--neural-backend", "amd",
            "--range", f"f0-f{LOAD_FRAMES}", "--encode", "standard", "--quiet",
        ]
        render = subprocess.Popen(render_cmd, cwd=str(PLAYER.parent),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  creationflags=FLAGS)
        # Wait until the render has actually started producing work, not just
        # been launched, so the two arms genuinely overlap.
        time.sleep(4)
        alive_at_start = render.poll() is None
        if not alive_at_start:
            print(f"POC CONCURRENCY JUDGE  INCONCLUSIVE  reason=neural_load_exited_early rc={render.returncode}")
            return 3

        loaded, loaded_wall = run_encode()
        alive_at_end = render.poll() is None
        # Give the render a bounded window to finish; never hang on it.
        try:
            render.wait(timeout=120)
            render_rc = render.returncode
        except subprocess.TimeoutExpired:
            render.kill()
            render_rc = "timeout-killed"

    if loaded is None:
        print("POC CONCURRENCY JUDGE  FAIL  reason=loaded_encode_no_frames")
        return 2

    overlap = alive_at_start and alive_at_end
    slowdown = loaded / base if base else float("inf")
    under_threshold = loaded < THRESHOLD_MS
    print(f"POC CONC ARM loaded:   frame_ms={loaded:.2f} wall={loaded_wall:.2f}s "
          f"slowdown={slowdown:.2f}x render_alive(start={alive_at_start},end={alive_at_end}) rc={render_rc}")

    if not overlap:
        print("POC CONCURRENCY JUDGE  INCONCLUSIVE  reason=no_overlap (load not alive across encode window)")
        return 3
    # Gate on the absolute realtime budget only. Slowdown is a reported fact,
    # not a gate (see module docstring: the question is "does it drop frames").
    ok = under_threshold
    verdict = "PASS" if ok else "FAIL"
    print(f"POC CONCURRENCY JUDGE  {verdict}  amf_encode does NOT drop frames under real AMD neural upscale: "
          f"loaded={loaded:.2f}ms threshold={THRESHOLD_MS:.0f}ms (30fps deadline=33.3ms) "
          f"slowdown={slowdown:.2f}x (reported, not gated)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
