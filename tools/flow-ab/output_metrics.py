"""Output-frame metrics for the motion-guide A/B (stage 2).

docs/MOTION-GUIDE-AB-zh.md leaves one question open: a better motion field need
not produce a better FSR 4 output, because FSR 4 has its own reprojection and
clamping. Answering it needs the *output frames*, not the field, and a metric
that can tell success from failure.

This module deliberately does NOT use a single head-line PSNR as the verdict.
docs/BENCHMARK-METRIC-VALIDITY-zh.md records why: on this footage the low band
holds 97-99% of the residual, so a real 2 dB mid-band win lands as +0.02 dB in
the head-line, and "smoother" wins flicker/SSIM/dE in every case. Four axes are
reported together:

    band.mid / band.high   correctness against a REAL high-resolution ground
                           truth (the frames the player's input was made from)
    detail                 RMS high band of output / truth; guards against
                           winning a temporal metric by blurring
    static_flicker         second temporal difference on regions the truth says
                           are static; the shimmer a viewer sees
    ghost_leak             projection of the output error onto the previous
                           frame's direction, on regions the truth says changed;
                           ~0.2 means 20% of the old frame is still in the new one

`--selftest` is the gate. It injects degradations of known strength into a real
film still and asserts that the metrics respond monotonically, that a blurred
shimmer arm is caught by `detail`, and that an unchanged arm is exactly inert.
Do not use these numbers on real dumps until that gate passes.

Dump format written by the player: out_NNNNN.rgba16f, tightly packed
R16G16B16A16_FLOAT linear light at m_outputW x m_outputH, plus manifest.txt.
Truth frames are 8-bit sRGB PNGs at the same size. Both are converted to a
common 8-bit sRGB luma scale before scoring.
"""
from __future__ import annotations

import argparse
import math
import os
import subprocess
import sys
import tempfile

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCHMARK = os.path.normpath(os.path.join(HERE, "..", "benchmark"))
if BENCHMARK not in sys.path:
    sys.path.insert(0, BENCHMARK)
import bands  # noqa: E402  - same three-band split the benchmark uses


# ---------------------------------------------------------------------------
# colour / bands
# ---------------------------------------------------------------------------

def luma(rgb: np.ndarray) -> np.ndarray:
    """Rec.709 luma on an 8-bit sRGB-like RGB float plane (N,H,W,3) -> (N,H,W)."""
    return (rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152
            + rgb[..., 2] * 0.0722).astype(np.float32)


def linear_to_srgb8(linear: np.ndarray) -> np.ndarray:
    """FP16 linear light -> 8-bit sRGB scale (not rounded: metrics want float)."""
    x = np.clip(linear, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x,
                    1.055 * np.power(x, 1.0 / 2.4) - 0.055) * 255.0


def srgb8_to_float(bgr: np.ndarray) -> np.ndarray:
    return bgr[..., ::-1].astype(np.float32)


def psnr(residual: np.ndarray) -> float:
    mse = float(np.mean(np.square(residual, dtype=np.float32), dtype=np.float64))
    return math.inf if mse <= 1e-12 else 10.0 * math.log10(255.0 * 255.0 / mse)


def split_batch(plane: np.ndarray) -> dict[str, np.ndarray]:
    """bands.split works on one 2-D luma plane; run it per frame so a whole clip
    cannot be mistaken by cv2 for a many-channel image."""
    return {name: np.stack([bands.split(plane[t])[name] for t in range(plane.shape[0])])
            for name in bands.BANDS}


def pooled(plane: np.ndarray, factor: int = 4) -> np.ndarray:
    """Mean-pool spatially, keeping time: a 2-pixel shimmy is not visible wobble."""
    n, h, w = plane.shape
    h -= h % factor
    w -= w % factor
    return plane[:, :h, :w].reshape(n, h // factor, factor, w // factor, factor).mean(axis=(2, 4))


# ---------------------------------------------------------------------------
# the four axes
# ---------------------------------------------------------------------------

def score(truth: np.ndarray, out: np.ndarray) -> dict:
    """truth, out: (N,H,W,3) float32 RGB on an 8-bit sRGB scale."""
    ty, oy = luma(truth), luma(out)
    tb, ob = split_batch(ty), split_batch(oy)

    result = {"psnr": psnr(oy - ty)}
    for name in bands.BANDS:
        result["band_" + name] = psnr(ob[name] - tb[name])
    result["detail"] = float(
        np.sqrt(np.mean(np.square(ob["high"], dtype=np.float32), dtype=np.float64))
        / max(1e-6, np.sqrt(np.mean(np.square(tb["high"], dtype=np.float32), dtype=np.float64))))

    # Static mask from the TRUTH, never from the arm: an arm that smears a moving
    # object into a flat plate must not be able to declare itself "static".
    still = (np.maximum(np.abs(ty[2:] - ty[1:-1]),
                        np.abs(ty[1:-1] - ty[:-2])) < 1.0)
    shake = np.abs(oy[2:] - 2.0 * oy[1:-1] + oy[:-2])
    static_cells = pooled(still.astype(np.float32)) > 0.99
    shake_cells = pooled(shake)
    result["static_flicker"] = (float(shake_cells[static_cells].mean())
                               if static_cells.any() else float("nan"))
    result["static_share"] = float(still.mean())

    # Ghost leak. d is what a leftover *previous* frame would add (old - new);
    # e is what the output actually got wrong. Their normalised projection is
    # the fraction of the old frame still present. The sign matters: using
    # (new - old) here made every injected trail read as a NEGATIVE leak, and
    # the first selftest caught it. The common spatial SR error gives a nonzero
    # baseline even on a clean arm; it is arm-independent and cancels when arms
    # are compared, so report deltas against a control arm, not the raw value.
    d = ty[:-1] - ty[1:]
    e = oy[1:] - ty[1:]
    changed = np.abs(d) > 8.0
    if changed.any():
        frac = np.clip((e * d) / (d * d + 1e-3), -1.0, 1.0)
        result["ghost_leak"] = float(frac[changed].mean())
        result["change_share"] = float(changed.mean())
    else:
        result["ghost_leak"] = float("nan")
        result["change_share"] = 0.0
    return result


# ---------------------------------------------------------------------------
# loaders
# ---------------------------------------------------------------------------

def load_rgba16f(directory: str, width: int, height: int, count: int,
                 prefix: str = "out_", stride: int | None = None,
                 start: int = 0) -> np.ndarray:
    """Read out_NNNNN.rgba16f and convert linear FP16 -> 8-bit sRGB RGB float."""
    if stride is None:
        desc = os.path.join(directory, "manifest.txt")
        if os.path.isfile(desc):
            with open(desc, "r", encoding="ascii") as handle:
                lines = [line.split() for line in handle if line.strip()]
            stride = int(lines[0][3]) if lines else width * 8
        else:
            stride = width * 8
    frames = np.empty((count, height, width, 3), dtype=np.float32)
    for index in range(count):
        path = os.path.join(directory, f"{prefix}{index + start:05d}.rgba16f")
        raw = np.fromfile(path, dtype=np.float16)
        expected = stride // 2 * height
        if raw.size != expected:
            raise SystemExit(f"{path}: {raw.size} halves, expected {expected}")
        linear = raw.reshape(height, stride // 2)[:, :width * 4].reshape(height, width, 4)[..., :3]
        frames[index] = linear_to_srgb8(linear.astype(np.float32))
    return frames


def load_truth_pngs(directory: str, count: int, start: int = 0) -> np.ndarray:
    frames = []
    for index in range(count):
        path = os.path.join(directory, f"truth_{index + start:05d}.png")
        image = cv2.imread(path, cv2.IMREAD_COLOR)
        if image is None:
            raise SystemExit(f"missing truth frame: {path}")
        frames.append(srgb8_to_float(image))
    return np.stack(frames).astype(np.float32)


# ---------------------------------------------------------------------------
# self-test: can these numbers tell success from failure?
# ---------------------------------------------------------------------------

def film_still(fixture: str, ffmpeg: str) -> np.ndarray:
    """One real 1920x1080 frame. Never a synthetic periodic test pattern:
    a translated periodic pattern matches itself at zero motion and makes every
    estimator look right (docs/MOTION-GUIDE-AB-zh.md)."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "still.png")
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                        "-i", fixture, "-vf", "select=eq(n\\,300)", "-frames:v", "1", path],
                       check=True)
        image = cv2.imread(path, cv2.IMREAD_COLOR)
    if image is None:
        raise SystemExit("could not extract a still from the fixture")
    return srgb8_to_float(image)


def make_sequence(still: np.ndarray, count: int = 24, pan: int = 2,
                  static_fraction: float = 0.25) -> np.ndarray:
    """Truth: a real film frame panned by `pan` px/frame, with a genuinely static
    left strip. The pan is what makes history reprojection matter; the strip is
    where shimmer is unambiguously a defect."""
    h, w, _ = still.shape
    cut = int(w * static_fraction)
    seq = np.empty((count, h, w, 3), dtype=np.float32)
    for t in range(count):
        shifted = np.roll(still, -pan * t, axis=1)
        shifted[:, :cut] = still[:, :cut]
        seq[t] = shifted
    return seq


def spatial_only(truth: np.ndarray, scale: int = 2) -> np.ndarray:
    """An arm-independent 'clean' stand-in for an SR output: down and back up.
    Real FSR 4 output will differ, but all arms share this fixed error, so the
    *differences* between arms are what the metrics read."""
    h, w = truth.shape[1:3]
    out = np.empty_like(truth)
    for t in range(truth.shape[0]):
        small = cv2.resize(truth[t], (w // scale, h // scale), interpolation=cv2.INTER_AREA)
        out[t] = cv2.resize(small, (w, h), interpolation=cv2.INTER_LANCZOS4)
    return out


def degrade(clean: np.ndarray, kind: str, strength: float) -> np.ndarray:
    out = clean.copy()
    n = out.shape[0]
    if kind == "ghost":
        for t in range(1, n):
            out[t] = (1.0 - strength) * clean[t] + strength * clean[t - 1]
    elif kind == "shimmer":
        # Spatially structured, zero-mean, alternating in time: a plain DC
        # offset survives Gaussian blur, so it cannot demonstrate the "a blurrier
        # arm wins flicker" trap. 4x4 blocks align with the 4x temporal pooling
        # below, so the wobble a viewer sees is preserved by pooling but removed
        # by a sigma=1 blur. Second difference is 4*strength per block.
        h, w = out.shape[1:3]
        yy, xx = np.indices((h, w))
        block = np.where(((yy // 4) + (xx // 4)) % 2 == 0, 1.0, -1.0).astype(np.float32)
        for t in range(n):
            out[t] += (1.0 if t % 2 else -1.0) * strength * block[..., None]
    elif kind == "blur":
        for t in range(n):
            out[t] = cv2.GaussianBlur(out[t], (0, 0), strength)
    elif kind == "shimmer_blur":
        out = degrade(out, "shimmer", strength)
        out = degrade(out, "blur", 1.0)
    elif kind != "clean":
        raise SystemExit("unknown degradation: " + kind)
    return out


def selftest(fixture: str, ffmpeg: str, size: tuple[int, int]) -> int:
    still = film_still(fixture, ffmpeg)
    if size:
        still = cv2.resize(still, size, interpolation=cv2.INTER_AREA)
    truth = make_sequence(still)
    clean = spatial_only(truth)

    arms = [("clean (control)", clean), ("identical (noise floor)", clean.copy())]
    for a in (0.10, 0.20, 0.30):
        arms.append((f"ghost {int(a*100)}%", degrade(clean, "ghost", a)))
    for s in (1.0, 2.0, 4.0):
        arms.append((f"shimmer {s:g}", degrade(clean, "shimmer", s)))
    for s in (0.5, 1.0, 2.0):
        arms.append((f"blur s={s:g}", degrade(clean, "blur", s)))
    arms.append(("shimmer 4 + blur s=1 (trap)", degrade(clean, "shimmer_blur", 4.0)))

    rows = [(name, score(truth, frames)) for name, frames in arms]
    columns = [("psnr", "psnr"), ("band_mid", "mid"), ("band_high", "high"),
               ("detail", "detail"), ("static_flicker", "flicker"),
               ("ghost_leak", "ghost")]
    width = max(len(name) for name, _ in rows) + 2
    print("".ljust(width) + "".join(c[1].rjust(11) for c in columns))
    for name, row in rows:
        print(name.ljust(width) + "".join(f"{row[k]:11.4f}" for k, _ in columns))
    by = dict(rows)
    print(f"\nstatic share {by['clean (control)']['static_share']:.3f}, "
          f"changed share {by['clean (control)']['change_share']:.3f}")

    failures = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)
        print(("  OK   " if condition else "  FAIL ") + message)

    print("\ndiscrimination checks")
    base = by["clean (control)"]["ghost_leak"]
    ghosts = [by[f"ghost {int(a*100)}%"] for a in (0.10, 0.20, 0.30)]
    check(all(ghosts[i]["ghost_leak"] < ghosts[i + 1]["ghost_leak"] for i in (0, 1)),
          "ghost_leak rises monotonically with injected trail strength")
    check(all(ghosts[i]["psnr"] > ghosts[i + 1]["psnr"] for i in (0, 1)),
          "head-line PSNR falls monotonically with injected trail strength")
    deltas = [g["ghost_leak"] - base for g in ghosts]
    check(all(0.5 * a < d < 1.1 * a for a, d in zip((0.10, 0.20, 0.30), deltas)),
          "ghost_leak delta over control reads the injected fraction (calibration)")
    shimmers = [by[f"shimmer {s:g}"] for s in (1.0, 2.0, 4.0)]
    check(all(shimmers[i]["static_flicker"] < shimmers[i + 1]["static_flicker"]
              for i in (0, 1)),
          "static_flicker rises monotonically with injected shimmer")
    blur = by["blur s=1"]
    check(blur["detail"] < 0.80, "blur is caught by detail (not mistaken for a win)")
    trap = by["shimmer 4 + blur s=1 (trap)"]
    check(trap["static_flicker"] < by["shimmer 4"]["static_flicker"]
          and trap["detail"] < 0.80,
          "blurring a shimmering arm lowers flicker BUT the detail guard catches it")
    same = by["identical (noise floor)"]
    check(all(abs(same[k] - by["clean (control)"][k]) < 1e-9
              for k, _ in columns), "unchanged arm is exactly inert")

    print("\n" + ("SELFTEST PASS" if not failures else "SELFTEST FAIL: " + "; ".join(failures)))
    return 0 if not failures else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="validate the metrics on injected degradations and exit")
    ap.add_argument("--fixture", default=os.path.join(BENCHMARK, "fixtures", "demo-capture-20260912.mp4"))
    ap.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", r"F:\rvc\ffmpeg\bin\ffmpeg.exe"))
    ap.add_argument("--size", default="", help="WxH for the selftest still (default: fixture native)")
    ap.add_argument("--truth", help="directory of truth_NNNNN.png")
    ap.add_argument("--arm", help="directory of out_NNNNN.rgba16f")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--count", type=int, default=12)
    ap.add_argument("--start", type=int, default=12,
                    help="source frame index of the first dumped frame (DLSS5_*_DUMP_FROM)")
    ap.add_argument("--offset", type=int, default=0,
                    help="global truth-vs-dump frame shift, from --calibrate")
    ap.add_argument("--calibrate", action="store_true",
                    help="scan offsets -3..3 and report the best luma alignment, then exit")
    args = ap.parse_args()

    if args.selftest:
        size = tuple(int(x) for x in args.size.lower().split("x")) if args.size else ()
        return selftest(args.fixture, args.ffmpeg, size)
    if not (args.truth and args.arm and args.width and args.height):
        raise SystemExit("need --truth --arm --width --height, or --selftest")
    out = load_rgba16f(args.arm, args.width, args.height, args.count, start=args.start)
    # Stage 1 found the player's render-frame counter is not the decoder's frame
    # number (real60 needed -1). Never assume they agree: measure the global
    # integer shift against the truth before scoring any arm.
    if args.calibrate:
        print(f"{'offset':>8} {'luma_mse':>12} {'head_psnr':>12}")
        best = (float("inf"), 0)
        for off in range(-3, 4):
            try:
                trial = load_truth_pngs(args.truth, args.count, start=args.start + off)
            except SystemExit:
                continue
            residual = luma(trial) - luma(out)
            mse = float(np.mean(np.square(residual, dtype=np.float32), dtype=np.float64))
            head = math.inf if mse <= 1e-12 else 10.0 * math.log10(255.0 * 255.0 / mse)
            print(f"{off:8d} {mse:12.3f} {head:12.4f}")
            if mse < best[0]:
                best = (mse, off)
        print(f"best offset = {best[1]} (luma MSE {best[0]:.3f})")
        return 0
    truth = load_truth_pngs(args.truth, args.count, start=args.start + args.offset)
    if truth.shape != out.shape:
        raise SystemExit(f"shape mismatch: truth {truth.shape}, arm {out.shape}")
    row = score(truth, out)
    for key, value in row.items():
        print(f"{key:20s} {value:12.5f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
