"""Score a dumped motion field on real footage, where no ground truth exists.

The test is warp consistency: if the field is right, taking this frame's content
back to where the field says it was lands on the previous frame. So warp the
frame by the field and compare with the frame before it. A field of zeros leaves
the whole frame-to-frame change unexplained; a wrong field explains it worse than
nothing.

  python warp.py --clip real60 --arms cpu,ffx,off --offset 1

`--offset` maps a dumped field index to the source frame it was resolved from.
Playback drops one frame while the window comes up, and the resolver counts
resolve calls, not decoded frames - the offset is calibrated once per clip by
picking the value that minimises the residual of the FFX arm, which is the arm
this tool has independent evidence for (the synthetic clips score it against a
known answer).
"""
import argparse
import json
import os

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.environ.get("FLOWAB_RUNS", os.path.join(HERE, "runs"))
CLIPS = os.environ.get("FLOWAB_CLIPS", os.path.join(HERE, "clips"))


def load_field(path, w, h, pitch):
    raw = np.fromfile(path, dtype=np.uint8)
    rows = raw.reshape(h, pitch)[:, :w * 4]
    halves = rows.copy().view(np.float16).reshape(h, w, 2)
    return halves[..., 0].astype(np.float32), halves[..., 1].astype(np.float32)


def decode(path):
    cap = cv2.VideoCapture(path)
    frames = []
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        frames.append(frame.astype(np.float32) @ np.array([0.114, 0.587, 0.299], np.float32))
    cap.release()
    return frames


def score(directory, frames, offset):
    manifest = os.path.join(directory, "manifest.txt")
    if not os.path.isfile(manifest):
        return None
    out = []
    for line in open(manifest):
        row = line.split()
        if not row:
            continue
        idx, w, h, pitch = int(row[0]), int(row[1]), int(row[2]), int(row[3])
        path = os.path.join(directory, f"flow_{idx:05d}.rg16f")
        if not os.path.isfile(path):
            continue
        t = idx + offset
        if t < 1 or t >= len(frames):
            continue
        vx, vy = load_field(path, w, h, pitch)
        cur, prev = frames[t], frames[t - 1]
        if cur.shape != vx.shape:
            continue
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        # v takes this pixel back to where its content sat in the previous frame,
        # so the previous frame is cur sampled at p - v. Getting this backwards
        # does not look like a sign error, it looks like the field being wrong:
        # it doubles the apparent displacement.
        warped = cv2.remap(cur, xs - vx, ys - vy, cv2.INTER_LINEAR,
                           borderMode=cv2.BORDER_REPLICATE)
        mag = np.hypot(vx, vy)
        moving = mag > 0.5
        still = mag <= 0.5
        base = np.abs(cur - prev)
        out.append({
            "index": idx,
            "cov": float(moving.mean()),
            # Mean absolute luma error, 0-255, of the explained frame.
            "res": float(np.abs(warped - prev).mean()),
            # The same, with no motion at all: what the field has to beat.
            "base": float(base.mean()),
            # Only where the arm claims motion: an all-zero field is not allowed
            # to look good by only being scored where nothing happens.
            "res_moving": float(np.abs(warped - prev)[moving].mean()) if moving.any() else float("nan"),
            "base_moving": float(base[moving].mean()) if moving.any() else float("nan"),
            "res_still": float(np.abs(warped - prev)[still].mean()) if still.any() else float("nan"),
            "base_still": float(base[still].mean()) if still.any() else float("nan"),
        })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True)
    ap.add_argument("--arms", default="cpu,ffx,off")
    ap.add_argument("--offset", type=int, default=1)
    ap.add_argument("--calibrate", action="store_true",
                    help="print the residual for every offset, for the FFX arm")
    args = ap.parse_args()

    clip = os.path.join(CLIPS, args.clip + ".mp4")
    frames = decode(clip)
    print(f"{args.clip}: {len(frames)} frames decoded")
    if args.calibrate:
        for k in range(-1, 3):
            rows = score(os.path.join(RUNS, args.clip, "ffx"), frames, k)
            if rows:
                print(f"  offset {k:+d}: residual {np.mean([r['res'] for r in rows]):.4f}"
                      f"  base {np.mean([r['base'] for r in rows]):.4f}")
        return
    for arm in args.arms.split(","):
        rows = score(os.path.join(RUNS, args.clip, arm), frames, args.offset)
        if not rows:
            print(f"  {arm:<4} (no frames)")
            continue
        m = {k: float(np.nanmean([r[k] for r in rows])) for k in rows[0] if k != "index"}
        gain = 1.0 - m["res"] / m["base"] if m["base"] else float("nan")
        gain_mv = (1.0 - m["res_moving"] / m["base_moving"]
                   if m["base_moving"] == m["base_moving"] and m["base_moving"] else float("nan"))
        print(f"  {arm:<4} cov={m['cov']:5.3f}  res={m['res']:6.3f} vs base={m['base']:6.3f}"
              f"  (-{gain * 100:5.1f}%)  where it claims motion: res={m['res_moving']:6.3f}"
              f" vs {m['base_moving']:6.3f} (-{gain_mv * 100:5.1f}%)  n={len(rows)}")


if __name__ == "__main__":
    main()
