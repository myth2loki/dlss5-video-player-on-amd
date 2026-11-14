#!/usr/bin/env python3
"""Aggregate stage-2 output-frame metrics and apply the pre-registered verdict.

Pairs dumps with truth BY DECODED FRAME NUMBER (written into manifest.txt by
the player), not by a global integer offset. Every arm scores the same
contiguous run of frame numbers, so a dropped frame cannot turn into a fake
delta. Repeated runs of the same arm are combined per-pixel by median.

The verdict rule was written down BEFORE the run in docs/MOTION-GUIDE-AB-zh.md:

    >= 2/3 of shots same sign, each |delta| > 0.05 dB on BOTH band_mid and
    band_high; and no trade-off guard (detail / ghost / flicker) trips.

Three pre-conditions can force INCONCLUSIVE and are never hidden:
  * alignment.json missing / identity gate failed / any best_shift != 0
  * the same-configuration repeat was not run (noise floor unmeasured)
  * measured noise floor exceeds the 0.05 dB threshold

"Cannot be determined" and "no difference" are different results.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import align  # noqa: E402
import output_metrics as om  # noqa: E402

THRESHOLD_DB = 0.05
DETAIL_GUARD = 0.002          # 0.2% of the truth's high-band RMS
GHOST_GUARD = 0.01
FLICKER_GUARD = 0.01


def guards_trip(ffx: dict, cpu: dict) -> list[str]:
    bad = []
    if ffx["detail"] < cpu["detail"] - DETAIL_GUARD:
        bad.append("detail")
    if ffx["ghost_leak"] > cpu["ghost_leak"] + GHOST_GUARD:
        bad.append("ghost")
    if ffx["static_flicker"] > cpu["static_flicker"] * 1.05 + FLICKER_GUARD:
        bad.append("flicker")
    return bad


def fmt(value: float) -> str:
    return "     n/a" if not math.isfinite(value) else f"{value:8.4f}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.environ.get("FLOWAB_OUTPUTS",
                                                    os.path.join(HERE, "outputs")))
    ap.add_argument("--runs", default="",
                    help="comma-separated run roots; same-arm frames are median-combined")
    ap.add_argument("--arms", default="ffx,cpu,off")
    ap.add_argument("--shots", default="")
    args = ap.parse_args()

    default_runs = os.path.join(args.out, "runs")
    runs = [p.strip() for p in args.runs.split(",") if p.strip()] or [default_runs]
    with open(os.path.join(args.out, "truth.json"), "r", encoding="utf-8") as handle:
        meta = json.load(handle)
    fps = float(meta.get("fps", 24))
    shots = args.shots.split() if args.shots else [s["name"] for s in meta["shots"]]
    arms = [a.strip() for a in args.arms.split(",") if a.strip()]

    align_path = os.path.join(args.out, "alignment.json")
    if not os.path.isfile(align_path):
        raise SystemExit("run diagnose_alignment.py first (alignment.json missing)")
    with open(align_path, "r", encoding="utf-8") as handle:
        alignment = json.load(handle)

    gate_problems = []
    if not alignment.get("identity_ok", False):
        gate_problems.append("identity/offset gate FAILED")
    if not alignment.get("noise_available", False):
        gate_problems.append("same-configuration repeat not measured")
    for shot, entry in alignment.get("shots", {}).items():
        for arm, check in entry.get("offset_check", {}).items():
            if not check.get("aligned", check.get("best_shift", -99) == 0):
                gate_problems.append(
                    f"{shot}/{arm} not aligned (shift={check.get('best_shift')}, "
                    f"pts_dev={check.get('pts_deviation_ticks')})")

    noise_db = 0.0
    for rec in alignment.get("noise", {}).values():
        r1, r2 = rec["run1"], rec["run2"]
        noise_db = max(noise_db, abs(r1["band_mid"] - r2["band_mid"]),
                       abs(r1["band_high"] - r2["band_high"]))

    results: dict[str, dict] = {}
    print(f"{'shot':<10} {'frames':>6} {'arm':<4} {'band_mid':>9} {'band_high':>10} "
          f"{'detail':>8} {'flicker':>8} {'ghost':>8} {'head':>8}")
    for shot in shots:
        numbers = alignment.get("shots", {}).get(shot, {}).get("numbers", [])
        # Median-combining several passes is only fair if every pass supplies
        # exactly the same frame numbers; recompute the common run across all
        # passes (a pass with a different dropped-frame pattern has a shifted
        # window, e.g. 36..47 instead of 37..48).
        if len(runs) > 1:
            numbers, detail = align.common_numbers(arms, shot, runs, fps)
            if not numbers:
                print(f"{shot:<10} SKIP  no common frame run across passes: {detail}")
                continue
        if not numbers:
            print(f"{shot:<10} SKIP  no common frame-number run")
            continue
        truth = align.load_truth(os.path.join(args.out, "truth", shot), numbers)
        entry: dict = {"numbers": numbers, "arms": {}}
        for arm in arms:
            try:
                out = align.load_arm(runs, shot, arm, numbers, fps)
            except SystemExit as exc:
                print(f"{shot:<10} {arm:<4} SKIP  {exc}")
                continue
            row = om.score(truth, out)
            entry["arms"][arm] = row
            print(f"{shot:<10} {len(numbers):>6} {arm:<4} {fmt(row['band_mid'])} "
                  f"{fmt(row['band_high'])} {fmt(row['detail'])} "
                  f"{fmt(row['static_flicker'])} {fmt(row['ghost_leak'])} "
                  f"{fmt(row['psnr'])}")
            del out
        results[shot] = entry

    print("\nffx - cpu (positive = FFX better on that axis)")
    signs: list[str] = []
    for shot, entry in results.items():
        if "ffx" not in entry["arms"] or "cpu" not in entry["arms"]:
            continue
        ffx, cpu = entry["arms"]["ffx"], entry["arms"]["cpu"]
        dm = ffx["band_mid"] - cpu["band_mid"]
        dh = ffx["band_high"] - cpu["band_high"]
        bad = guards_trip(ffx, cpu)
        if bad:
            sign, why = "0", "trade-off: " + ",".join(bad)
        elif dm > THRESHOLD_DB and dh > THRESHOLD_DB:
            sign, why = "+", "both bands > 0.05 dB"
        elif dm < -THRESHOLD_DB and dh < -THRESHOLD_DB:
            sign, why = "-", "both bands < -0.05 dB"
        else:
            sign, why = "0", "no measurable difference"
        signs.append(sign)
        print(f"{shot:<10} d_mid={dm:+.4f} d_high={dh:+.4f} "
              f"d_detail={ffx['detail'] - cpu['detail']:+.5f}  {sign}  {why}")
        entry.update(delta_mid=dm, delta_high=dh, sign=sign, reason=why)

    need = math.ceil(2.0 * len(signs) / 3.0) if signs else 999
    if gate_problems:
        verdict = "INCONCLUSIVE: " + "; ".join(sorted(set(gate_problems)))
    elif noise_db > THRESHOLD_DB:
        verdict = (f"INCONCLUSIVE: same-configuration noise {noise_db:.4f} dB exceeds "
                   f"the {THRESHOLD_DB} dB threshold; arm deltas are not separable")
    elif signs.count("+") >= need:
        verdict = "FFX better than CPU on mid/high bands (>=2/3 shots)"
    elif signs.count("-") >= need:
        verdict = "FFX worse than CPU on mid/high bands (>=2/3 shots)"
    else:
        verdict = "no measurable output-frame difference under the pre-registered rule"
    print(f"\nnoise floor {noise_db:.4f} dB (same config, twice) vs threshold {THRESHOLD_DB} dB")
    print(f"verdict ({signs.count('+')}+ / {signs.count('-')}- / "
          f"{signs.count('0')}0, need {need}): {verdict}")
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump({"verdict": verdict, "threshold_db": THRESHOLD_DB,
                   "noise_floor_db": noise_db, "runs": runs,
                   "alignment_gate": alignment.get("gate"),
                   "gate_problems": gate_problems,
                   "rule": ">=2/3 shots same sign, both bands > 0.05 dB, no trade-off guard",
                   "shots": results}, handle, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
