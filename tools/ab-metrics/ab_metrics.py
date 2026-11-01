"""Score two AMD neural runtimes against each other and against a clean reference.

Companion to tests/AmdNrAbCompare.cpp, which renders one raw frame sequence
through both runtimes and dumps every frame. This reads those dumps and the
reference they were built from, and prints the numbers docs/AMD-MOCHIZUKI-
COEXIST-PLAN-zh.md section 13 quotes.

PSNR against the reference is printed but must not decide anything on its own: a
denoiser is supposed to rebuild what compression destroyed, so every correct
thing it does moves pixels away from the reference. Both runtimes score several
dB *worse* than the untouched input while visibly doing their job. The columns
that answer the question without the reference are `temporal` and `flicker`.

Expected layout:

    <workdir>/truth.bgra                  clean reference, w*h*4*n
    <workdir>/input.bgra                  the degraded sequence the runtimes got
    <workdir>/<frames>/lmxxf/NNNNN.bgra
    <workdir>/<frames>/mochizuki/NNNNN.bgra

    python ab_metrics.py <workdir> <width> <height> [--frames frames] [--count 66]

Requires numpy.
"""
import argparse
import os

import numpy as np


def load_sequence(path, width, height, count):
    raw = np.fromfile(path, dtype=np.uint8)
    expected = width * height * 4 * count
    if raw.size != expected:
        raise SystemExit(f"{path}: {raw.size} bytes, expected {expected}")
    frames = raw.reshape(count, height, width, 4)
    # BGRA -> RGB, float32 in [0,255]
    return frames[:, :, :, 2::-1].astype(np.float32)


def load_directory(directory, width, height, count):
    frames = np.empty((count, height, width, 3), dtype=np.float32)
    frame_bytes = width * height * 4
    for index in range(count):
        path = os.path.join(directory, f"{index:05d}.bgra")
        if not os.path.isfile(path):
            raise SystemExit(f"missing frame: {path}")
        raw = np.fromfile(path, dtype=np.uint8)
        if raw.size != frame_bytes:
            raise SystemExit(f"{path}: {raw.size} bytes, expected {frame_bytes}")
        frames[index] = raw.reshape(height, width, 4)[:, :, 2::-1].astype(np.float32)
    return frames


def luma(frames):
    return frames[:, :, :, 0] * 0.2126 + frames[:, :, :, 1] * 0.7152 + frames[:, :, :, 2] * 0.0722


def highpass(plane):
    """5x5 box blur subtracted - the detail the eye reads as texture.

    Cumulative-sum box filter so a whole sequence stays vectorised; a per-pixel
    loop over 66 frames of 720p would be minutes rather than seconds.
    """
    pad = 2
    padded = np.pad(plane, ((0, 0), (pad, pad), (pad, pad)), mode="edge")
    cumulative = np.cumsum(np.cumsum(padded, axis=1), axis=2)
    cumulative = np.pad(cumulative, ((0, 0), (1, 0), (1, 0)), mode="constant")
    k = 2 * pad + 1
    total = (cumulative[:, k:, k:] - cumulative[:, :-k, k:]
             - cumulative[:, k:, :-k] + cumulative[:, :-k, :-k])
    return plane - total / (k * k)


def gradient(plane):
    gx = np.zeros_like(plane)
    gy = np.zeros_like(plane)
    gx[:, :, 1:-1] = (plane[:, :, 2:] - plane[:, :, :-2]) * 0.5
    gy[:, 1:-1, :] = (plane[:, 2:, :] - plane[:, :-2, :]) * 0.5
    return np.sqrt(gx * gx + gy * gy)


def pooled(plane, factor=4):
    """Mean-pool spatially, keeping the temporal axis.

    Flicker a viewer notices is low frequency: a 2-pixel shimmy averages out of
    a 4x4 cell, a whole-region brightness wobble does not. Pooling is what turns
    "the pixels moved" into "the picture wobbled".
    """
    count, height, width = plane.shape
    return plane.reshape(count, height // factor, factor, width // factor, factor).mean(axis=(2, 4))


def jitter(plane):
    """Mean |second difference in time|, i.e. how hard the picture is being shaken."""
    return float(np.abs(plane[2:] - 2.0 * plane[1:-1] + plane[:-2]).mean())


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return float("inf") if mse <= 1e-12 else 10.0 * np.log10(255.0 * 255.0 / mse)


def score(name, candidate, truth, truth_hf):
    residual = candidate - truth
    error_plane = residual.mean(axis=3)

    # Flat regions of the reference: a denoiser has nothing legitimate to
    # reconstruct there, so movement is noise it left or added.
    flat = gradient(luma(truth)).mean(axis=0) < 2.0
    flat_noise = float(np.sqrt(np.mean(error_plane[:, flat] ** 2))) if flat.any() else float("nan")

    # Temporal movement against the reference's own movement. Above 1 is
    # shakiness the content does not have - a defect one frame cannot show.
    move_candidate = float(np.mean(np.abs(np.diff(luma(candidate), axis=0))))
    move_truth = float(np.mean(np.abs(np.diff(luma(truth), axis=0))))
    temporal = move_candidate / max(1e-6, move_truth)

    luma_candidate = luma(candidate)
    return {
        "arm": name,
        "psnr": psnr(candidate, truth),
        "flat_noise": flat_noise,
        "flicker": float(np.mean(np.std(error_plane, axis=0))),
        "temporal": temporal,
        "detail": float(np.sqrt(np.mean(highpass(luma_candidate) ** 2)) / max(1e-6, truth_hf)),
        "colour": float(np.mean(np.abs(candidate.mean(axis=(0, 1, 2)) - truth.mean(axis=(0, 1, 2))))),
        "low_flicker": jitter(pooled(luma_candidate)),
        "jitter": jitter(luma_candidate),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("workdir")
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("--frames", default="frames", help="frame directory under workdir")
    parser.add_argument("--count", type=int, default=66, help="frames per arm")
    args = parser.parse_args()

    width, height, count = args.width, args.height, args.count
    truth = load_sequence(os.path.join(args.workdir, "truth.bgra"), width, height, count)
    degraded = load_sequence(os.path.join(args.workdir, "input.bgra"), width, height, count)

    arms = [("clean reference", truth), ("degraded input", degraded)]
    for runtime in ("lmxxf", "mochizuki"):
        directory = os.path.join(args.workdir, args.frames, runtime)
        if os.path.isdir(directory):
            arms.append((runtime, load_directory(directory, width, height, count)))
        else:
            print(f"note: no frames for {runtime} under {directory}, skipped")

    truth_hf = np.sqrt(np.mean(highpass(luma(truth)) ** 2))
    rows = [score(name, candidate, truth, truth_hf) for name, candidate in arms]

    columns = [
        ("psnr", "psnr"), ("flat_noise", "flat_noise"), ("flicker", "flicker"),
        ("temporal", "temporal"), ("detail", "detail"), ("colour", "colour"),
        ("jitter", "jitter"), ("low_flicker", "low_flicker"),
    ]
    label_width = max(len(row["arm"]) for row in rows) + 2
    print("".ljust(label_width) + "".join(title.rjust(12) for _, title in columns))
    for row in rows:
        line = row["arm"].ljust(label_width)
        line += "".join(f"{row[key]:12.3f}" for key, _ in columns)
        print(line)

    print("\nlow_flicker is the low-frequency one (4x pooled) - the one a viewer sees.")
    print("psnr is printed for completeness and must not decide anything alone: a"
          " reconstructing denoiser has to lose it against the untouched input.")


if __name__ == "__main__":
    main()
