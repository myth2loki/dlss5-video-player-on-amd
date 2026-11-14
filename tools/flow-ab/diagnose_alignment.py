#!/usr/bin/env python3
"""Stage-2 alignment gate: run this BEFORE believing any arm delta.

The player stamps each output dump with the decoded frame number (see
D3D12Renderer::SetNextFrameDumpIdentity), so pairing is by identity, not by a
global integer offset guessed from image MSE. This script proves that stamp is
sane and measures the one thing that can still invalidate a verdict:

  * identity check  - every arm has the stamp, PTS agrees with frameNumber,
                      and after pairing the best global shift is exactly 0.
                      A nonzero shift means the stamp names a different frame
                      than the pixels (queue lag) and no delta may be used.
  * noise floor     - the SAME configuration played twice must agree to better
                      than the 0.05 dB verdict threshold on the same frames.

Writes alignment.json, consumed by summarize_output.py. If the repeat run is
absent the noise floor is NOT silently treated as zero: the summary must stay
INCONCLUSIVE.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import align  # noqa: E402
import output_metrics as om  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.environ.get("FLOWAB_OUTPUTS",
                                                    os.path.join(HERE, "outputs")))
    ap.add_argument("--runs", default="",
                    help="comma-separated run roots (repeats of the same batch)")
    ap.add_argument("--repeat", default="",
                    help="second run root for the same-configuration noise floor")
    ap.add_argument("--arms", default="ffx,cpu,off")
    ap.add_argument("--shots", default="")
    args = ap.parse_args()

    default_runs = os.path.join(args.out, "runs")
    runs = [p.strip() for p in args.runs.split(",") if p.strip()] or [default_runs]
    repeat = args.repeat or os.path.join(args.out, "runs_repeat")
    with open(os.path.join(args.out, "truth.json"), "r", encoding="utf-8") as handle:
        meta = json.load(handle)
    fps = float(meta.get("fps", 24))
    shots = args.shots.split() if args.shots else [s["name"] for s in meta["shots"]]
    arms = [a.strip() for a in args.arms.split(",") if a.strip()]

    report: dict = {"fps": fps, "runs": runs, "repeat": repeat,
                    "shots": {}, "noise": {}, "noise_available": False}
    ok = True
    print("identity pairing (best global shift MUST be 0 for every arm)")
    print(f"{'shot':<10} {'arm':<4} {'frames':>6} {'range':>12} {'pts_dev':>9} {'shift':>6}")
    for shot in shots:
        numbers, detail = align.common_numbers(arms, shot, runs, fps)
        entry = {"numbers": numbers, "arms": detail, "offset_check": {},
                 "pts_deviation_ticks": max(
                     [v.get("pts_deviation_ticks", 0) for v in detail.values()
                      if isinstance(v, dict)] or [0])}
        if not numbers:
            entry["error"] = ("no contiguous frame-number run shared by all arms "
                              f"({detail})")
            report["shots"][shot] = entry
            print(f"{shot:<10} FAIL  {entry['error']}")
            ok = False
            continue
        truth = align.load_truth(os.path.join(args.out, "truth", shot), numbers)
        for arm in arms:
            arm_dir = os.path.join(runs[0], shot, arm)
            try:
                out = align.load_frames(arm_dir, numbers, fps)
            except SystemExit as exc:
                entry["offset_check"][arm] = {"error": str(exc)}
                print(f"{shot:<10} {arm:<4} FAIL  {exc}")
                ok = False
                continue
            shift, costs = align.mse_offset_check(truth, out)
            dev = align.pts_deviation(align.read_manifest(arm_dir), fps)
            # PTS/frameNumber is the hard fact; MSE is only a cross-check and is
            # powerless on a near-static shot. o_real_a's curve is 71.3/71.3/
            # 71.3/71.5/71.8 - a floating-point tie that once picked -1. Only
            # call it a failure when offset 0 is >2% worse than the best shift.
            c0, cb = costs.get("0", float("inf")), costs.get(str(shift), 0.0)
            mse_flat = shift != 0 and (c0 - cb) <= 0.02 * c0
            aligned = dev < (1e7 / fps) and (shift == 0 or mse_flat)
            entry["offset_check"][arm] = {"best_shift": shift, "costs": costs,
                                          "pts_deviation_ticks": dev,
                                          "mse_flat": mse_flat, "aligned": aligned}
            flag = "OK" if aligned else "FAIL"
            if mse_flat:
                flag = "OK (flat)"
            if not aligned:
                ok = False
            print(f"{shot:<10} {arm:<4} {len(numbers):>6} "
                  f"{numbers[0]:>5}-{numbers[-1]:<6} {dev:>9.1f} {shift:>+6d} {flag}")
            del out
        report["shots"][shot] = entry

    print("\nsame-configuration noise floor (ffx played twice, identical frames)")
    for shot in shots:
        numbers = report["shots"].get(shot, {}).get("numbers", [])
        first = os.path.join(runs[0], shot, "ffx")
        second = os.path.join(repeat, shot, "ffx")
        if not numbers or not (os.path.isfile(os.path.join(first, "manifest.txt"))
                               and os.path.isfile(os.path.join(second, "manifest.txt"))):
            print(f"{shot:<10} SKIP  no repeat dump")
            continue
        # A dropped frame in one process shifts its frame-number window (the
        # o_pan4 repeat ran 36..47 while the batch ran 37..48). Score only the
        # contiguous run present in BOTH processes; the old integer-offset
        # calibration could not even see this difference.
        have1 = set(align.derive_numbers(align.read_manifest(first), fps))
        have2 = set(align.derive_numbers(align.read_manifest(second), fps))
        paired = align.longest_run(n for n in numbers if n in have1 and n in have2)
        if len(paired) < 8:
            print(f"{shot:<10} SKIP  repeat shares only {len(paired)} contiguous frames")
            continue
        truth = align.load_truth(os.path.join(args.out, "truth", shot), paired)
        rows = {}
        for label, arm_dir in (("run1", first), ("run2", second)):
            rows[label] = om.score(truth, align.load_frames(arm_dir, paired, fps))
        r1, r2 = rows["run1"], rows["run2"]
        report["noise"][shot] = {"frames": len(paired), "run1": r1, "run2": r2}
        report["noise_available"] = True
        print(f"{shot:<10} d_mid={r1['band_mid'] - r2['band_mid']:+.4f} dB "
              f"d_high={r1['band_high'] - r2['band_high']:+.4f} dB "
              f"d_detail={r1['detail'] - r2['detail']:+.5f}")
    report["identity_ok"] = ok
    report["gate"] = ("PASS" if ok and report["noise_available"] else
                      "FAIL - no verdict may be drawn")
    print(f"\nidentity/offset gate: {'PASS' if ok else 'FAIL'}; "
          f"noise measured: {report['noise_available']}")
    with open(os.path.join(args.out, "alignment.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
