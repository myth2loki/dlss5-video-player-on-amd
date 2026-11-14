"""Offline probe: does FidelityFX Optical Flow's SCD add anything over the
player's existing scene-cut evidence?

Reproduces (approximately) the SCD divergence in
  ffx_opticalflow_generate_scd_histogram.h  (3x3 tiled 256-bin luma histograms)
  ffx_opticalflow_compute_scd_divergence.h (11-tap filter, 3 shifts, symmetric
                                            Jeffreys divergence, min over shifts)
and compares it with the global luma histogram intersection that the shipped
CPU cut classifier already uses as its weak arm. See docs/ARCHITECTURE.md,
"Scene-change output", for the conclusion this probe supports.

Ground truth comes from tools/benchmark/corpus.py, where every cut was confirmed
frame by frame. Build the two clips from the checked-in fixture first:

  ffmpeg -i tools/benchmark/fixtures/demo-capture-20260912.mp4 \
    -vf "select='between(n\\,15\\,116)',setpts=N/30/TB,crop=1362:766:502:126,\
scale=1920:1080:flags=lanczos,format=yuv420p" -frames:v 102 \
    -c:v libx264 -qp 0 real-film-cuts.mp4
  python tools/benchmark/scd_probe.py --clip real-film-cuts.mp4 \
    --cuts 20,47,70,87

  ffmpeg -i tools/benchmark/fixtures/demo-capture-20260912.mp4 \
    -vf "select='between(n\\,438\\,505)',setpts=N/30/TB,crop=1362:766:502:126,\
scale=1920:1080:flags=lanczos,format=yuv420p" -frames:v 68 \
    -c:v libx264 -qp 0 real-game-cuts.mp4
  python tools/benchmark/scd_probe.py --clip real-game-cuts.mp4 --cuts 32

The clips are intentionally not checked in: they are generated from the fixture
in seconds and are tens of megabytes each.
"""
import argparse
import json

import cv2
import numpy as np

KERNEL = np.array([
    0.0088122291, 0.027143577, 0.065114059, 0.12164907, 0.17699835,
    0.20056541, 0.17699835, 0.12164907, 0.065114059, 0.027143577,
    0.0088122291,
], dtype=np.float64)
DEFAULT_CHALLENGES = "2,4,7,13,15,18,84,85,86,29,33"
SCD_THRESHOLD = 0.45  # CrossedSceneChangeThreshold in the SDK


def tiled_histograms(frame):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    out = []
    for ny in range(3):
        for nx in range(3):
            tile = gray[ny * h // 3:(ny + 1) * h // 3,
                        nx * w // 3:(nx + 1) * w // 3]
            # Counts, NOT probabilities. The SDK accumulates pixel counts into
            # scdHistogram; the +1 in the divergence pass is a log(0) guard that
            # is negligible against counts of tens of thousands. Normalising here
            # before that +1 flattens every histogram to near-uniform and all
            # measured values collapse to ~1e-4.
            hist = np.bincount(tile.ravel(), minlength=256).astype(np.float64)
            padded = np.pad(hist, (5, 5), mode="edge")
            filtered = np.array([KERNEL @ padded[i:i + 11] for i in range(256)])
            filtered += 1.0
            filtered /= filtered.sum()
            out.append(filtered)
    global_hist = np.bincount(gray.ravel(), minlength=256).astype(np.float64)
    global_hist /= max(global_hist.sum(), 1.0)
    return out, global_hist


def divergence(current, previous):
    """Symmetric Jeffreys divergence -> resFloat, as in the SDK."""
    eps = 1e-12
    p = np.clip(current, eps, None)
    q = np.clip(previous, eps, None)
    s1 = float(np.sum(p * np.log(p / q)))
    s2 = float(np.sum(q * np.log(q / p)))
    return 1.0 - np.exp(-(abs(s1) + abs(s2)))


def scd_value(current_tiles, previous_tiles):
    # Three index shifts of the filtered histogram, mirroring iGlobalId.y 0/1/2.
    # Edges are filled from the nearest bin (the SDK writes a neutral 1.0 there
    # before renormalising). Take the minimum over shifts.
    per_shift = []
    for shift in (-1, 0, 1):
        vals = []
        for cur, prev in zip(current_tiles, previous_tiles):
            shifted = np.roll(cur, shift)
            if shift > 0:
                shifted[0] = cur[0]
            elif shift < 0:
                shifted[-1] = cur[-1]
            vals.append(divergence(shifted, prev))
        per_shift.append(float(np.mean(vals)))
    return min(per_shift)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True)
    ap.add_argument("--json")
    ap.add_argument("--cuts", default="20,47,70,87",
                    help="comma-separated first-frame-of-new-shot indices")
    ap.add_argument("--challenges", default=DEFAULT_CHALLENGES,
                    help="comma-separated non-cut frames that must NOT fire")
    args = ap.parse_args()
    cut_indices = {int(x) for x in args.cuts.split(",") if x.strip()}
    challenges = {int(x) for x in args.challenges.split(",") if x.strip()}

    cap = cv2.VideoCapture(args.clip)
    frames = []
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        frames.append(frame)
    cap.release()
    print(f"{len(frames)} frames decoded")

    rows = []
    previous = None
    for i, frame in enumerate(frames):
        tiles, global_hist = tiled_histograms(frame)
        if previous is None:
            previous = (tiles, global_hist)
            continue
        prev_tiles, prev_global = previous
        rows.append({
            "pair": i,               # transition from frame i-1 to frame i
            "scd": scd_value(tiles, prev_tiles),
            "overlap": float(np.minimum(global_hist, prev_global).sum()),
            "cut": i in cut_indices,
            "challenge": i in challenges,
        })
        previous = (tiles, global_hist)

    cuts = [r for r in rows if r["cut"]]
    others = [r for r in rows if not r["cut"]]
    print("\nlabelled cuts (SCD threshold 0.45):")
    for r in cuts:
        print(f"  pair {r['pair']:3d}  scd={r['scd']:.4f}  "
              f"{'FIRE' if r['scd'] > SCD_THRESHOLD else 'miss'}"
              f"   histogram_overlap={r['overlap']:.4f}")
    if cuts and others:
        scd_cut_min = min(r["scd"] for r in cuts)
        scd_other_max = max(r["scd"] for r in others)
        ov_cut_max = max(r["overlap"] for r in cuts)
        ov_other_min = min(r["overlap"] for r in others)
        print(f"\nmin SCD at cuts        {scd_cut_min:.4f}")
        print(f"max SCD elsewhere      {scd_other_max:.4f}  "
              f"(separable: {scd_cut_min > scd_other_max})")
        print(f"max overlap at cuts    {ov_cut_max:.4f}")
        print(f"min overlap elsewhere  {ov_other_min:.4f}  "
              f"(separable: {ov_cut_max < ov_other_min})")
    fired = [r["pair"] for r in rows if r["scd"] > SCD_THRESHOLD]
    print(f"\nSCD fires on {len(fired)} pairs: {fired}")
    print(f"false positives: {[p for p in fired if p not in cut_indices]}")
    print("\nchallenge frames (should NOT fire):")
    for r in rows:
        if r["challenge"]:
            print(f"  pair {r['pair']:3d}  scd={r['scd']:.4f}  overlap={r['overlap']:.4f}"
                  f"  {'FIRE' if r['scd'] > SCD_THRESHOLD else ''}")
    if args.json:
        json.dump(rows, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
