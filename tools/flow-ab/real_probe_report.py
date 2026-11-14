#!/usr/bin/env python3
"""Real-footage probe report: compare the three motion-guide arms on a real clip.

The stage-2 A/B scores arms against a synthetic truth image, so it can only say
what happened on pans it generated. Real footage has no truth frame, so this
report answers a narrower and still useful set of questions:

  * did all three arms actually render, and on which decoded frames,
  * how far apart are the arms on a frame they all reached,
  * are those differences on content or only on the letterbox boundary,
  * how bright were the pixels that differ (a 90-level difference in the black
    bars is not a visible artifact; the same difference on a lit edge is),
  * a PNG per arm plus 5x zoom crops of the worst block, for eyeballing.

No truth, no PSNR, no verdict word. Anything printed here is a description of
what the dumps contain, not a quality score.

    python tools/flow-ab/real_probe_report.py --dir 'C:\\Temp\\dlss5-real\\runs\\real_film'
"""
import argparse
import json
import os
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import align  # noqa: E402

ARMS = ("ffx", "cpu", "off")
LETTERBOX_LUMA = 16   # a row whose brightest pixel is darker than this is bar
EDGE_MARGIN = 8       # rows next to a bar boundary are excluded from content stats
BIG_DIFF = 6          # sRGB8 levels; below this nothing survives compression noise


def load_u8(arm_dir, numbers, fps):
    frames = align.load_frames(arm_dir, numbers, fps)
    return np.clip(frames, 0, 255).astype(np.uint8)


def content_rows(img_u8):
    """Rows inside the picture area, excluding the bars and their edge rows."""
    row_max = img_u8.max(axis=2).max(axis=1)
    black = np.where(row_max < LETTERBOX_LUMA)[0]
    h = img_u8.shape[0]
    top = black[black < h // 2]
    bottom = black[black >= h // 2]
    y0 = int(top.max()) + 1 + EDGE_MARGIN if top.size else 0
    y1 = int(bottom.min()) - EDGE_MARGIN if bottom.size else h
    return max(0, y0), min(h, y1), (int(top.min()), int(top.max())) if top.size else None, \
        (int(bottom.min()), int(bottom.max())) if bottom.size else None


def write_png(path, rgb):
    import cv2
    cv2.imwrite(path, cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="runs dir holding <arm>/manifest.txt")
    ap.add_argument("--fps", type=float, default=24.0)
    ap.add_argument("--png-dir", default=None, help="defaults to <dir>/../png")
    ap.add_argument("--max-frames", type=int, default=3)
    args = ap.parse_args()

    png_dir = args.png_dir or os.path.join(os.path.dirname(os.path.abspath(args.dir)), "png")
    os.makedirs(png_dir, exist_ok=True)

    manifests, numbers = {}, {}
    for arm in ARMS:
        arm_dir = os.path.join(args.dir, arm)
        if not os.path.isfile(os.path.join(arm_dir, "manifest.txt")):
            print("%-4s MISSING manifest" % arm)
            continue
        manifests[arm] = align.read_manifest(arm_dir)
        numbers[arm] = [int(r["frame_number"]) for r in manifests[arm]]

    present = [a for a in ARMS if a in numbers]
    if len(present) < 2:
        raise SystemExit("need at least two arms with dumps")

    print("arm  frames                    generations        resets")
    for arm in present:
        gens = sorted({r["history"] for r in manifests[arm]})
        resets = sorted({int(r["reset"]) for r in manifests[arm]})
        print("%-4s %s  gen=%s  reset=%s" %
              (arm, " ".join("%d" % n for n in numbers[arm]), gens, resets))

    common = sorted(set.intersection(*(set(numbers[a]) for a in present)))
    print("common frame numbers: %s" % (common or "none"))
    if not common:
        print("no frame reached by every arm - nothing to compare")
        return 1

    pick = common[:args.max_frames]
    report = {"dir": args.dir, "arms": present, "common": common,
              "compared": pick, "frames": {}}

    for arm in present:
        frames = load_u8(os.path.join(args.dir, arm), numbers[arm][:args.max_frames], args.fps)
        for i, n in enumerate(numbers[arm][:args.max_frames]):
            write_png(os.path.join(png_dir, "%s_%05d.png" % (arm, n)), frames[i])

    ref = "ffx" if "ffx" in present else present[0]
    for n in pick:
        stack = {a: load_u8(os.path.join(args.dir, a), [n], args.fps)[0] for a in present}
        y0, y1, top, bottom = content_rows(stack[ref])
        per = {}
        for arm in present:
            if arm == ref:
                continue
            diff = np.abs(stack[ref].astype(np.int16) - stack[arm].astype(np.int16)).max(axis=2)
            inside = diff[y0:y1]
            luma = stack[ref].mean(axis=2)[y0:y1]
            big = inside > BIG_DIFF
            lit_big = big & (luma > 60)
            entry = {
                "mean": float(inside.mean()),
                "p99": float(np.percentile(inside, 99)),
                "p9999": float(np.percentile(inside, 99.99)),
                "max": int(inside.max()),
                "frac_gt2": float((inside > 2).mean()),
                "frac_gt6": float(big.mean()),
                "frac_gt6_lit": float(lit_big.mean()),
                "max_lit": int(inside[lit_big].max()) if lit_big.any() else 0,
            }
            per["%s_vs_%s" % (ref, arm)] = entry
        report["frames"][str(n)] = {"content_rows": [y0, y1 - 1],
                                    "letterbox": {"top": top, "bottom": bottom},
                                    "diffs": per}

        if pick.index(n) == 0:
            frames = [stack[a] for a in present]
            write_png(os.path.join(png_dir, "full_%05d.png" % n), stack[ref])
            diff = np.abs(stack[ref].astype(np.int16) - stack[present[-1]].astype(np.int16)).max(axis=2)
            hot = Counter(zip((np.where(diff > BIG_DIFF)[0] // 32), (np.where(diff > BIG_DIFF)[1] // 32)))
            if hot:
                import cv2
                (yy, xx), count = hot.most_common(1)[0]
                cy, cx = yy * 32, xx * 32
                ya, yb = max(y0, cy - 32), min(y1, cy + 64)
                xa, xb = max(0, cx - 32), min(stack[ref].shape[1], cx + 64)
                tiles = []
                for a in present:
                    crop = cv2.cvtColor(stack[a][ya:yb, xa:xb], cv2.COLOR_RGB2BGR)
                    tiles.append(cv2.resize(crop, None, fx=5, fy=5, interpolation=cv2.INTER_NEAREST))
                sep = np.full((tiles[0].shape[0], 8, 3), 255, np.uint8)
                row = tiles[0]
                for t in tiles[1:]:
                    row = np.hstack([row, sep, t])
                for i, a in enumerate(present):
                    cv2.putText(row, a.upper(), (i * (tiles[0].shape[1] + 8) + 8, 34),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 3)
                cv2.imwrite(os.path.join(png_dir, "zoom_block_%05d.png" % n), row)
                report["hot_block"] = {"frame": int(n), "y": int(cy), "x": int(cx),
                                       "count": int(count)}

    for n, entry in report["frames"].items():
        print("frame %s  content rows %d..%d  letterbox top=%s bottom=%s" %
              (n, entry["content_rows"][0], entry["content_rows"][1],
               entry["letterbox"]["top"], entry["letterbox"]["bottom"]))
        for key, d in entry["diffs"].items():
            print("  %-14s mean=%.3f p99=%.2f p99.99=%.2f max=%d  >2lvl=%.4f%%  "
                  ">6lvl=%.4f%%  >6lvl&lit=%.4f%%  max_lit=%d" %
                  (key, d["mean"], d["p99"], d["p9999"], d["max"], 100 * d["frac_gt2"],
                   100 * d["frac_gt6"], 100 * d["frac_gt6_lit"], d["max_lit"]))

    out = os.path.join(args.dir, "real_probe.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    print("pngs + %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
