#!/usr/bin/env python3
"""Frame-identity alignment for the stage-2 output-frame A/B.

The player now writes the decoded source frame's FrameIdentity into
output-dump manifest.txt (see SetNextFrameDumpIdentity). That replaces the
global integer offset scan, which chose a different offset per player process
and could flip the sign of an arm delta.

manifest.txt columns (the last four are new; older five-column dumps still
parse and are reported as unidentified):

    index width height rowPitch flowDumpIndex
    [frameNumber pts100ns historyGeneration reset]

Truth PNGs are named by the SAME 0-based decoded frame number, so a dump row
pairs with truth_NNNNN.png directly. Dropped frames no longer shift anything:
a dropped frame is simply missing, and only the longest run of frame numbers
present in EVERY arm is scored (the temporal metrics need a contiguous
sequence).
"""
from __future__ import annotations

import os
from typing import Iterable

import cv2
import numpy as np

import output_metrics as om


def read_manifest(arm_dir: str) -> list[dict]:
    path = os.path.join(arm_dir, "manifest.txt")
    with open(path, "r", encoding="ascii") as handle:
        lines = [line.split() for line in handle if line.strip()]
    rows = []
    for parts in lines:
        if len(parts) < 5:
            raise SystemExit(f"bad manifest row in {path}: {parts!r}")
        row = {"index": int(parts[0]), "w": int(parts[1]), "h": int(parts[2]),
               "pitch": int(parts[3]), "flow": int(parts[4]),
               "frame_number": 0, "pts": 0, "history": 0, "reset": 0,
               "has_identity": False}
        if len(parts) >= 9:
            row.update(frame_number=int(parts[5]), pts=int(parts[6]),
                       history=int(parts[7]), reset=int(parts[8]),
                       has_identity=True)
        elif len(parts) >= 7:
            # Tolerate a transitional build that wrote only frameNumber/PTS.
            row.update(frame_number=int(parts[5]), pts=int(parts[6]),
                       has_identity=True)
        rows.append(row)
    return rows


def identified(rows: list[dict]) -> bool:
    """A dump with no identity has all-zero frame numbers; PTS alone is enough
    to reconstruct them (fixed-rate truth clips), but an all-zero PTS means the
    player build predates the stamp."""
    return bool(rows) and all(r["has_identity"] for r in rows) and any(
        r["frame_number"] or r["pts"] for r in rows)


def derive_numbers(rows: list[dict], fps: float) -> list[int]:
    """Prefer the decoder's frameNumber; fall back to PTS when it is absent."""
    if all(r["frame_number"] for r in rows):
        return [r["frame_number"] for r in rows]
    step = 1e7 / float(fps)
    return [int(round(r["pts"] / step)) for r in rows]


def pts_deviation(rows: list[dict], fps: float) -> float:
    """Largest |pts - frameNumber*step| in 100ns ticks. A bad stamp (e.g. the
    renderer lagging the decoder by one frame) shows up here, not in the MSE."""
    step = 1e7 / float(fps)
    return max((abs(r["pts"] - r["frame_number"] * step) for r in rows), default=0.0)


def longest_run(numbers: Iterable[int]) -> list[int]:
    """Longest contiguous run, so the temporal metrics see a real time axis."""
    ordered = sorted(set(int(n) for n in numbers))
    if not ordered:
        return []
    best, run = [], [ordered[0]]
    for value in ordered[1:]:
        if value == run[-1] + 1:
            run.append(value)
        else:
            if len(run) > len(best):
                best = run
            run = [value]
    return best if len(best) >= len(run) else run


def common_numbers(arms: list[str], shot: str, runs: list[str], fps: float,
                   minimum: int = 8) -> tuple[list[int], dict]:
    """Frame numbers present in every arm of every repeated run, trimmed to the
    longest contiguous run. Reports which arms are missing frames."""
    per_arm: dict[str, set[int]] = {}
    detail: dict = {}
    for root in runs:
        for arm in arms:
            arm_dir = os.path.join(root, shot, arm)
            if not os.path.isfile(os.path.join(arm_dir, "manifest.txt")):
                detail.setdefault("missing", []).append(arm_dir)
                return [], detail
            rows = read_manifest(arm_dir)
            if not identified(rows):
                detail.setdefault("unidentified", []).append(arm_dir)
                return [], detail
            nums = set(derive_numbers(rows, fps))
            detail[arm_dir] = {"count": len(nums), "first": min(nums),
                               "last": max(nums), "pts_deviation_ticks":
                               pts_deviation(rows, fps)}
            per_arm[arm_dir] = nums
    sets = list(per_arm.values())
    if not sets:
        return [], detail
    common = set.intersection(*sets)
    run = longest_run(common)
    detail["common_count"] = len(common)
    detail["run_length"] = len(run)
    detail["run"] = [run[0], run[-1]] if run else []
    return (run if len(run) >= minimum else []), detail


def load_frames(arm_dir: str, numbers: list[int], fps: float) -> np.ndarray:
    """Load out_NNNNN.rgba16f for exactly `numbers`, in that order."""
    rows = read_manifest(arm_dir)
    by_number = {n: r for n, r in zip(derive_numbers(rows, fps), rows)}
    missing = [n for n in numbers if n not in by_number]
    if missing:
        raise SystemExit(f"{arm_dir}: missing frame numbers {missing[:8]}")
    w, h = rows[0]["w"], rows[0]["h"]
    stride = rows[0]["pitch"]
    out = np.empty((len(numbers), h, w, 3), dtype=np.float32)
    for i, number in enumerate(numbers):
        path = os.path.join(arm_dir, f"out_{by_number[number]['index']:05d}.rgba16f")
        raw = np.fromfile(path, dtype=np.float16)
        expected = stride // 2 * h
        if raw.size != expected:
            raise SystemExit(f"{path}: {raw.size} halves, expected {expected}")
        linear = raw.reshape(h, stride // 2)[:, :w * 4].reshape(h, w, 4)[..., :3]
        out[i] = om.linear_to_srgb8(linear.astype(np.float32))
    return out


def load_arm(runs: list[str], shot: str, arm: str, numbers: list[int],
             fps: float) -> np.ndarray:
    """One arm, optionally repeated: per-frame pixel median across runs.

    The median is taken on the SRGB-CONVERTED planes, not on the metrics: a
    single bad cold-start frame must not be able to move a whole shot's score.
    """
    stacks = []
    for root in runs:
        arm_dir = os.path.join(root, shot, arm)
        if not os.path.isfile(os.path.join(arm_dir, "manifest.txt")):
            continue
        try:
            stacks.append(load_frames(arm_dir, numbers, fps))
        except SystemExit as exc:
            print(f"  skip {arm_dir}: {exc}")
    if not stacks:
        raise SystemExit(f"no usable dump for {shot}/{arm}")
    return stacks[0] if len(stacks) == 1 else np.median(np.stack(stacks), axis=0)


def load_truth(truth_dir: str, numbers: list[int]) -> np.ndarray:
    frames = []
    for n in numbers:
        path = os.path.join(truth_dir, f"truth_{n:05d}.png")
        image = cv2.imread(path, cv2.IMREAD_COLOR)
        if image is None:
            raise SystemExit(f"missing truth frame: {path}")
        frames.append(om.srgb8_to_float(image))
    return np.stack(frames).astype(np.float32)


def mse_offset_check(truth: np.ndarray, out: np.ndarray) -> tuple[int, dict]:
    """After frame-number pairing the best global shift MUST be zero. A nonzero
    result means the stamp names a different frame than the pixels (queue lag,
    a one-frame presentation delay), and the deltas are not trustworthy."""
    probe = om.luma(out)[:, ::4, ::4]
    truth_luma = om.luma(truth)[:, ::4, ::4]
    n = len(probe)
    costs = {}
    for off in range(-2, 3):
        if off >= 0:
            shifted, target = truth_luma[off:], probe[:max(0, n - off)]
        else:
            shifted, target = truth_luma[:n + off], probe[-off:]
        k = min(len(shifted), len(target))
        if k < 4:
            continue
        costs[str(off)] = float(np.mean(np.square(
            shifted[:k] - target[:k], dtype=np.float32), dtype=np.float64))
    best = min(costs, key=lambda k: costs[k])
    return int(best), costs
