#!/usr/bin/env python3
"""Stage-2 truth material: a REAL 1920x1080 ground truth and its 2x input clip.

The player's SR switch upscales a local clip 2x (1280x520 -> 2560x1040 in the
stage-1 log), so a 960x540 input gives a 1920x1080 FSR 4 output. Every truth
frame is the high-resolution frame the corresponding input frame was made from;
no arm is used as another arm's reference.

Frames are numbered by SOURCE FRAME, 0-based. The player now writes that same
decoded frame number into output-dump manifest.txt, so truth_NNNNN.png pairs
with out_NNNNN.rgba16f by identity (see align.py) - no global integer offset
scan. 96 frames are generated so the dump can start at frame 60: the same-config
noise floor decayed monotonically with warm-up length (o_pan2 0.369 dB at
frame 36 -> 0.074 dB at frame 48), so the batch now waits for the FSR 4 temporal
history to settle before taking a frame.

The downsample is done in LINEAR light, matching the player: it decodes sRGB to
linear before FSR 4 sees the frame, and our dumped FP16 is linear too. Doing the
reduction in sRGB would bake a different low-frequency baseline into every arm.

Inputs are encoded lossless RGB (libx264rgb -qp 0), as in stage 1: codec noise
would be noise all three arms inherit, and it would differ per run.

    python gen_truth.py --out <directory>

Outputs <out>/truth/<shot>/truth_NNNNN.png, <out>/clips/<shot>.mp4 and
<out>/truth.json. Override FFMPEG for another ffmpeg build.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCHMARK = os.path.normpath(os.path.join(HERE, "..", "benchmark"))
# Ship with the repo instead of pointing at a machine-specific rvc install: the
# old absolute default was missing on this host and made clip encoding fail with
# FileNotFoundError after truth PNGs had already been written. Override with the
# FFMPEG environment variable when testing another build.
FFMPEG = os.environ.get("FFMPEG", os.path.normpath(os.path.join(
    HERE, "..", "..", "external", "ffmpeg", "bin", "ffmpeg.exe")))

OUT_W, OUT_H = 1920, 1080
IN_W, IN_H = 960, 540
FRAMES = 96
FPS = 24
DUMP_FROM = 60      # after warm-up; earlier batches used 12 and 36
DUMP_COUNT = 12
PAD = 800  # >= max pan (8 px/frame) x (frames-1), so the window never runs off

# Six shots, so the >=2/3-same-sign rule has something to work with. Four are
# synthetic pans at 1/2/4/8 OUTPUT pixels per frame - the range where stage 1
# measured the CPU estimator going silent while FFX still reports motion. Two
# are real film motion with no synthetic movement at all.
SHOTS = [
    {"name": "o_pan1", "kind": "pan", "step": 1, "source_frame": 200},
    {"name": "o_pan2", "kind": "pan", "step": 2, "source_frame": 200},
    {"name": "o_pan4", "kind": "pan", "step": 4, "source_frame": 200},
    {"name": "o_pan8", "kind": "pan", "step": 8, "source_frame": 200},
    # 96-frame real windows: a whole-fixture scan (6-frame-step luma delta)
    # found these two quiet, non-flat stretches. The old 300/470 anchors only
    # had a quiet 36-frame window - at 96 frames they straddle an unlabelled
    # cut (cost 32.6), which would reset FSR 4 history mid-dump.
    {"name": "o_real_a", "kind": "real", "start": 120},
    {"name": "o_real_b", "kind": "real", "start": 282},
]


def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.04045, x / 12.92,
                    np.power(np.maximum((x + 0.055) / 1.055, 0.0), 2.4))


def linear_to_srgb(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92,
                    1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def read_frame(cap: cv2.VideoCapture, index: int) -> np.ndarray:
    cap.set(cv2.CAP_PROP_POS_FRAMES, index)
    ok, frame = cap.read()
    if not ok or frame is None:
        raise SystemExit(f"cannot read fixture frame {index}")
    if frame.shape[:2] != (OUT_H, OUT_W):
        frame = cv2.resize(frame, (OUT_W, OUT_H), interpolation=cv2.INTER_LANCZOS4)
    return frame


def pan_frame(plate: np.ndarray, step: int, t: int) -> np.ndarray:
    """Crop a shifted window out of a reflect-padded plate (no black borders).

    Integer output-pixel shifts keep the truth sharp; the sub-pixel information
    lives in the 2x downsample that produces the input instead.
    """
    ext = cv2.copyMakeBorder(plate, PAD, PAD, PAD, PAD, cv2.BORDER_REFLECT)
    dx = step * t
    return ext[PAD:PAD + OUT_H, PAD + dx:PAD + dx + OUT_W].copy()


def to_input(truth_bgr: np.ndarray) -> np.ndarray:
    rgb = cv2.cvtColor(truth_bgr, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    low = cv2.resize(srgb_to_linear(rgb), (IN_W, IN_H), interpolation=cv2.INTER_AREA)
    srgb = np.clip(linear_to_srgb(low) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return cv2.cvtColor(srgb, cv2.COLOR_RGB2BGR)


def encode_clip(path: str, frames: list[np.ndarray]) -> None:
    proc = subprocess.Popen(
        [FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
         "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{IN_W}x{IN_H}",
         "-r", str(FPS), "-i", "-", "-c:v", "libx264rgb", "-qp", "0",
         "-preset", "veryfast", path],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    assert proc.stdin is not None
    for frame in frames:
        proc.stdin.write(np.ascontiguousarray(frame, dtype=np.uint8).tobytes())
    proc.stdin.close()
    err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
    if proc.wait() != 0:
        raise SystemExit(f"ffmpeg failed for {path}: {err[-400:]}")


def stable_start(cap: cv2.VideoCapture, wanted: int, frames: int) -> int:
    """Pick the quietest `frames`-frame window near `wanted` (avoid cuts)."""
    best, best_cost = wanted, float("inf")
    for start in range(max(0, wanted - 60), min(600, wanted + 61), 12):
        cost = 0.0
        prev = None
        for i in range(start, start + frames, 6):
            frame = cv2.resize(read_frame(cap, i), (240, 135))
            small = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY).astype(np.float32)
            if prev is not None:
                cost = max(cost, float(np.mean(np.abs(small - prev))))
            prev = small
        if cost < best_cost:
            best, best_cost = start, cost
    if best_cost > 12.0:
        raise SystemExit(f"no quiet {frames}-frame window near {wanted} (cost {best_cost:.1f})")
    return best


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.environ.get("FLOWAB_OUTPUTS",
                                                    os.path.join(HERE, "outputs")))
    ap.add_argument("--fixture", default=os.path.join(
        BENCHMARK, "fixtures", "demo-capture-20260912.mp4"))
    ap.add_argument("--frames", type=int, default=FRAMES)
    args = ap.parse_args()

    truth_root = os.path.join(args.out, "truth")
    clip_root = os.path.join(args.out, "clips")
    os.makedirs(truth_root, exist_ok=True)
    os.makedirs(clip_root, exist_ok=True)

    cap = cv2.VideoCapture(args.fixture)
    if not cap.isOpened():
        raise SystemExit(f"cannot open fixture: {args.fixture}")
    frames = args.frames
    manifest = {"size": [OUT_W, OUT_H], "input_size": [IN_W, IN_H],
                "frames": frames, "fps": FPS, "dump_from": DUMP_FROM,
                "dump_count": DUMP_COUNT, "shots": []}
    for shot in SHOTS:
        if shot["kind"] == "pan":
            plate = read_frame(cap, shot["source_frame"])
            truth = [pan_frame(plate, shot["step"], t) for t in range(frames)]
            meta = dict(shot, motion=f"pan {shot['step']} output px/frame")
        else:
            start = stable_start(cap, shot["start"], frames)
            truth = [read_frame(cap, start + t) for t in range(frames)]
            meta = dict(shot, start=start, motion="real film motion (no cut)")
        shot_dir = os.path.join(truth_root, shot["name"])
        os.makedirs(shot_dir, exist_ok=True)
        for t, frame in enumerate(truth):
            cv2.imwrite(os.path.join(shot_dir, f"truth_{t:05d}.png"), frame)
        encode_clip(os.path.join(clip_root, shot["name"] + ".mp4"),
                    [to_input(frame) for frame in truth])
        manifest["shots"].append(meta)
        print(f"{shot['name']:<10} {meta['motion']:<32} {frames} frames "
              f"{OUT_W}x{OUT_H} truth -> {IN_W}x{IN_H} input")
    cap.release()
    with open(os.path.join(args.out, "truth.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
    print(f"wrote {len(SHOTS)} shots to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
