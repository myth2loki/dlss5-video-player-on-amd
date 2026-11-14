"""Score a dumped motion field against a motion that is known exactly.

Reads what the player wrote (flow_NNNNN.rg16f + manifest.txt) for one arm of one
clip and reports, per frame:

  epe      mean |v_est - v_true| in DLSS-input pixels - the headline number
  p50/p95  the same distribution, because a field can be right on average and
           wrong exactly where it matters (edges, small objects)
  cov      fraction of pixels carrying a vector at all. A field of zeros has
           infinite EPE and 0 coverage; the two together are what separate
           "found nothing" from "found it wrong"
  within   fraction of pixels within half a pixel - the threshold under which
           reprojection in FSR 4's history is visually free

The convention of the field (does +y mean "content moved down" or "up") is not
assumed: it is measured once on pan4, where every pixel has the same known
displacement, and then fixed for the whole run by --sign.
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get("FLOWAB_RUNS", os.path.join(HERE, "runs"))
# Eroded by more than one frame of patch travel, so a one-frame misalignment in
# `frame` cannot move an edge into the scored region.
EDGE = 12


def load_field(path, w, h, row_pitch):
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size != row_pitch * h:
        raise SystemExit(f"{path}: {raw.size} bytes, expected {row_pitch * h}")
    rows = raw.reshape(h, row_pitch)[:, :w * 4]
    halves = rows.copy().view(np.float16).reshape(h, w, 2)
    return halves[..., 0].astype(np.float32), halves[..., 1].astype(np.float32)


def ground_truth(clip, spec, frame, w, h):
    """True displacement of every pixel of `frame`, in DLSS-input pixels.

    The sign convention here is the field's own: the vector that takes a pixel of
    this frame back to where its content was in the previous one. `--sign` flips
    both components for the opposite convention.
    """
    if spec["kind"] == "pan":
        vx = np.full((h, w), spec["dx"], np.float32)
        vy = np.full((h, w), spec["dy"], np.float32)
        return vx, vy
    if spec["kind"] == "zoom":
        s = spec["scale"]
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        # Frame `frame` is the plate scaled by s**frame, so the content at (x,y)
        # was at c + (p - c)/s one frame earlier.
        k = 1.0 / s - 1.0
        return (xs - w / 2) * k, (ys - h / 2) * k
    if spec["kind"] == "layers":
        top, bottom, screen_speed = spec["box"]
        vx = np.zeros((h, w), np.float32)
        vy = np.zeros((h, w), np.float32)
        # The patch starts at x=300 and slides at `screen_speed` px per frame; its
        # own content moves with it, so inside the patch the vector is -screen_speed
        # (current-to-previous), and outside it the plate is still.
        lo = int(round(300 + screen_speed * frame)) + EDGE
        hi = lo + 220 - 2 * EDGE
        vx[top + EDGE:bottom - EDGE, max(lo, 0):min(max(hi, 0), w)] = spec["dx"]
        return vx, vy
    raise SystemExit(f"unknown clip kind {spec['kind']}")


def arm_stats(directory, spec, sign, lattice):
    manifest = os.path.join(directory, "manifest.txt")
    if not os.path.isfile(manifest):
        return None
    rows = [line.split() for line in open(manifest) if line.strip()]
    frames = []
    for row in rows:
        idx, w, h, pitch = int(row[0]), int(row[1]), int(row[2]), int(row[3])
        sx = float(row[6])
        resolved = int(row[8]) if len(row) > 8 else -1
        grid_fill = float(row[9]) if len(row) > 9 else float('nan')
        path = os.path.join(directory, f"flow_{idx:05d}.rg16f")
        if not os.path.isfile(path):
            continue
        vx, vy = load_field(path, w, h, pitch)
        # The field is already in DLSS-input pixels (the resolve pass applied the
        # motion scale), so the ground truth has to be brought to the same unit.
        tw, th = int(spec["width"] * sx), int(spec["height"] * sx)
        gx, gy = ground_truth(None, spec, idx, tw, th)
        gx *= sx * sign[0]
        gy *= sx * sign[1]
        if lattice > 1:
            sl = (slice(lattice // 2, None, lattice), slice(lattice // 2, None, lattice))
            vx, vy, gx, gy = vx[sl], vy[sl], gx[sl], gy[sl]
        err = np.hypot(vx - gx, vy - gy)
        mag = np.hypot(vx, vy)
        tmag = np.hypot(gx, gy)
        frames.append({
            "index": idx,
            "epe": float(err.mean()),
            "p50": float(np.median(err)),
            "p95": float(np.percentile(err, 95)),
            "cov": float((mag > 0.25).mean()),
            "within": float((err < 0.5).mean()),
            # Motion the arm reported where the truth is still: the failure mode
            # that makes a static shot crawl.
            "false": float(((mag > 1.0) & (tmag < 0.25)).mean()),
            "resolved": resolved,
            "grid": grid_fill,
        })
    return frames


def summarize(frames):
    if not frames:
        return None
    keys = ("epe", "p50", "p95", "cov", "within", "false", "resolved", "grid")
    out = {k: float(np.mean([f[k] for f in frames])) for k in keys}
    out["frames"] = len(frames)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True)
    ap.add_argument("--arms", default="cpu,ffx,off")
    ap.add_argument("--sign", default="1,1",
                    help="field convention; measured once on pan4")
    ap.add_argument("--lattice", type=int, default=16,
                    help="evaluate on this pixel lattice; 1 = every pixel")
    ap.add_argument("--json")
    args = ap.parse_args()

    truth = json.load(open(os.path.join(HERE, "truth.json")))
    spec = truth[args.clip]
    sign = tuple(float(x) for x in args.sign.split(","))
    report = {}
    for arm in args.arms.split(","):
        d = os.path.join(RUNS, args.clip, arm)
        frames = arm_stats(d, spec, sign, args.lattice)
        s = summarize(frames)
        report[arm] = s
        if s:
            print(f"  {arm:<4} epe={s['epe']:6.3f}  p50={s['p50']:5.2f}  p95={s['p95']:6.2f}"
                  f"  cov={s['cov']:5.3f}  within0.5={s['within']:5.3f}  false={s['false']:6.3f}"
                  f"  resolved={s['resolved']:4.2f}  grid={s['grid']:5.3f}  n={s['frames']}")
        else:
            print(f"  {arm:<4} (no frames)")
    if args.json:
        json.dump(report, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
