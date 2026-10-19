"""The AMD true-guide A/B, end to end, without a person deciding anything.

Four arms, three clips, one command. The arms are
``tools/benchmark/amd-guides.profile.json``'s: ``amd-cpu`` (the CPU block
matcher's field handed to the evaluator in memory), ``amd-cpu-nzmt`` (the same
with the zero-motion test on), ``amd-zmv`` (arm C's dumped depth with a zero
motion field, so the only difference from arm C is the motion field) and
``amd-cpu-replay`` (arm C's dump fed back through the file path, which must
render byte-identical - that is the proof the motion stride fix holds).

What makes it unattended rather than a checklist:

* the zero field is written per clip at that clip's own frame count and
  analysis grid, read from the manifest instead of counted by hand;
* the arms run in dependency order, because Z and the replay consume arm C's
  dump and would otherwise read an absent directory;
* the self-checks are parsed out of the worker log rather than eyeballed: the
  ``guides=``/``temporal=`` line says which arm rendered, ``guide-frames=``
  says the guides arrived every frame, and ``history=`` says whether the
  runtime's temporal model engaged;
* the round-trip proof compares the two arms' frame digests itself;
* the decision matrix is printed from analyze.py's metrics with the
  thresholds this file carries, not transcribed from a table by eye.

    python tools/benchmark/amdguides.py
    python tools/benchmark/amdguides.py --clips still-hold --skip-analyze

Exit code 0 means every arm rendered, every self-check passed and the
round trip was byte-identical. The A/B verdict is printed either way: it is a
measurement, not a gate.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from common import (BUILD, CORPUS, RELEASE_RUNTIME, RUNS, WORK, framemd5, load_manifest,
                    sequence_digest, write_json)
from cutmirror import analysis_grid

HERE = Path(__file__).resolve().parent
PROFILE_FILE = HERE / "amd-guides.profile.json"

# The arms in dependency order. amd-cpu dumps the guides the other two read, so
# it is not merely first for tidiness - Z and the replay are invalid without it.
ARMS = ["amd-cpu", "amd-cpu-nzmt", "amd-zmv", "amd-cpu-replay"]

# Clips chosen for what each one can answer. still-hold holds one frame, so it
# measures what the estimator does on a still; the two moving clips are where a
# motion field can pay off or cost. depth-pan is synthetic with a known answer,
# real-game-motion is the footage case.
DEFAULT_CLIPS = ["still-hold", "depth-pan", "real-game-motion"]

# The pre-registered thresholds, from docs/AMD-GUIDE-AB-VERIFICATION-zh.md section 6.
# PSNR inside 0.1 dB counts as a tie, because below that the encoder's own
# quantisation moves the number as much as the temporal model does.
PSNR_TIE_DB = 0.1


def preflight_worker() -> Path:
    """The worker binary the arms will actually launch, or an explanation.

    common.py's RELEASE_RUNTIME is `<build>/Release/neural-runtime`, where
    `<build>` is DLSS_BENCHMARK_BUILD or `build-upscaling/`. This checkout's
    build script (build-fsr4.bat) builds into `build-ninja/`, so on a fresh
    machine the default points at a directory that does not exist and every arm
    would fail identically with no hint. Checked once, here, where the message
    can name the fix.
    """
    worker = RELEASE_RUNTIME / "NeuralWorker.exe"
    if worker.exists():
        return worker
    alternatives = [BUILD / "build-ninja" / "Release" / "neural-runtime" / "NeuralWorker.exe",
                    Path("F:/dlss5-video/dlss5-video-amd/build-ninja/Release/neural-runtime/NeuralWorker.exe")]
    for candidate in alternatives:
        if candidate.exists():
            raise SystemExit(
                f"the worker is built at {candidate}, but the harness looks at {worker}.\n"
                f"Set DLSS_BENCHMARK_BUILD to the build directory (the parent of Release/):\n"
                f"    set DLSS_BENCHMARK_BUILD={candidate.parent.parent}")
    raise SystemExit(
        f"no NeuralWorker.exe under {RELEASE_RUNTIME}.\n"
        f"Build it first - on this machine that is build-fsr4.bat, which produces\n"
        f"    build-ninja\\Release\\neural-runtime\\NeuralWorker.exe\n"
        f"and then either set DLSS_BENCHMARK_BUILD to the build directory or copy that\n"
        f"neural-runtime tree next to the harness's expected path.")


def resolve_amd_runtime(explicit: Path | None) -> Path:
    """The runtime tree, the way src/AmdLmxxfRuntime.cpp resolves it.

    Explicit argument, then DLSS5_AMD_RUNTIME_DIR, then the module directory
    and its two parents. The worker is launched from a per-profile clone under
    benchmark-work/, so the checkout layout is three levels up from the exe -
    which is exactly what ResolveRuntimeDirectory's own candidate list walks.
    Failing here rather than at the first arm is the difference between one
    clear line and four identical render failures.
    """
    candidates = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("DLSS5_AMD_RUNTIME_DIR"):
        candidates.append(Path(os.environ["DLSS5_AMD_RUNTIME_DIR"]))
    base = RELEASE_RUNTIME.parent  # the build's Release/ directory
    candidates += [base / "amd-neural-runtime", base.parent / "amd-neural-runtime",
                   WORK / "amd-neural-runtime", WORK.parent / "amd-neural-runtime"]
    for candidate in candidates:
        if (candidate / "LmxxfNrRuntime.dll").exists():
            return candidate
    raise SystemExit(
        "no AMD runtime package found. Looked at:\n  " + "\n  ".join(str(c) for c in candidates) +
        "\nPass --amd-runtime <dir>, or set DLSS5_AMD_RUNTIME_DIR, or put amd-neural-runtime "
        "beside the neural runtime.")


def write_zero_fields(manifest: dict, clips: list[str], root: Path) -> dict:
    """One zero-motion directory per clip, at that clip's grid and frame count.

    Written through the same area_average the worker's file path applies, so
    the files mean the field they would mean if the evaluator read them at the
    grid. A zero field is invariant under that average, which is why the size
    does not have to be right - but the FRAME COUNT does: a short directory is
    a per-frame read failure, not a zero.
    """
    import numpy as np
    import guidefiles

    by_name = {c["name"]: c for c in manifest["clips"]}
    written = {}
    for name in clips:
        clip = by_name[name]
        gw, gh = analysis_grid(clip["width"], clip["height"], clip["fps"])
        motion = np.zeros((gh, gw, 2), np.float32)
        for frame in range(clip["frames"]):
            guidefiles.write_frame(root / name, frame, None, motion)
        written[name] = dict(frames=clip["frames"], grid=f"{gw}x{gh}")
        print(f"zero field: {name} {clip['frames']} frames at {gw}x{gh} -> {root / name}")
    return written


def run_arm(clip: str, arm: str, timeout: float, corpus: Path) -> dict:
    """Renders one arm through run.py's own machinery, so nothing diverges from
    a profile the harness would run by hand."""
    import run as runner
    profiles = runner.load_profiles(PROFILE_FILE)
    if arm not in profiles:
        raise SystemExit(f"{PROFILE_FILE} has no arm {arm!r}; it has {list(profiles)}")
    manifest = {c["name"]: c for c in load_manifest(corpus)["clips"]}
    return runner.run_one(manifest[clip], arm, profiles[arm], 1, timeout, corpus, False)


def _first(pattern: str, text: str, default: str = "") -> str:
    found = re.search(pattern, text)
    return found.group(1) if found else default


def _value(line: str, key: str, *next_markers: str) -> str:
    """The value of `key=` in a space-separated key=value log line, ending at the
    next marker. Written by index rather than by regex because the values contain
    spaces and parentheses ("estimator(cpu block matcher)") and a non-greedy group
    between two literals will not match when the first literal is not the one that
    appears first in the line.

    The markers are the literal separators the line actually uses - `depth:` is a
    colon in the log, not `depth=` - because a marker that is not present leaves
    `find` at -1 and the field would swallow the rest of the line.
    """
    start = line.find(key)
    if start < 0:
        return ""
    start = line.find("=", start) + 1
    if start == 0:
        return ""
    ends = [line.find(marker, start) for marker in next_markers]
    ends = [e for e in ends if e > start]
    end = min(ends) if ends else len(line)
    return line[start:end].strip()


def parse_log(directory: Path) -> dict:
    """The two log lines that decide whether an arm measured what it claims.

    The bring-up line names the arm (guides, zero-motion test, temporal mode)
    and the per-job line publishes guide-frames, lease-frames and history=.
    Both are the renderer's own account of itself; reading them is cheaper and
    more reliable than inferring the arm from the profile that asked for it.
    """
    found = dict(runtime="", guides="", temporal="", zero_motion_test="", guide_frames=None,
                 lease_frames=None, history="")
    for name in ("pass1-stdout.txt", "pass1-stdout-restart.txt"):
        path = directory / name
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "Neural backend:" in text:
            line = next(l for l in text.splitlines() if "Neural backend:" in l)
            found["runtime"] = _value(line, "runtime", " guides=")
            # `guides=motion:estimator(cpu block matcher) depth:...` - the value
            # contains spaces, so each field ends at the next separator the line
            # actually uses rather than at whitespace. \S+ would cut it at
            # "estimator(cpu" and every check downstream would compare the wrong
            # string. Note `depth:` is a colon in the log, not `depth=`.
            found["guides"] = _value(line, "guides", " depth:")
            found["zero_motion_test"] = _value(line, "zero-motion-test", " temporal=")
            found["temporal"] = "on" if " temporal=on" in line else "off"
        if "AMD neural render:" in text:
            line = next(l for l in text.splitlines() if "AMD neural render:" in l)
            found["guide_frames"] = int(_first(r"guide-frames=(\d+)", line, "0")) or None
            found["lease_frames"] = int(_first(r"lease-frames=(\d+)", line, "0")) or None
            found["history"] = _first(r"history=(\S+)", line)
    return found


def self_check(clip: dict, arm: str, log: dict) -> list[str]:
    """Why this arm's numbers would not mean anything. Empty is the pass.

    The two arms that hand the estimator's field over in memory are the ones
    whose whole claim is "the guides arrived every frame"; the file arms are
    checked on what they can show instead. Membership is by the exact arm name,
    not by prefix: `amd-cpu-replay` starts with `amd-cpu` but is a file arm, and
    a prefix test would hold a replay to the estimator's contract and report a
    healthy run as broken.
    """
    problems = []
    in_memory = arm in ("amd-cpu", "amd-cpu-nzmt")
    if not log["runtime"]:
        problems.append("no bring-up line in the worker log: the render may not have reached the backend")
    if in_memory and "estimator" not in log["guides"]:
        problems.append(f"guides={log['guides']!r}, expected the CPU estimator's field")
    if not in_memory and "estimator" in log["guides"]:
        problems.append(f"guides={log['guides']!r}, expected a file field, not the estimator")
    if in_memory and log["temporal"] != "on":
        problems.append("temporal=off: the runtime's history would not run, so this arm is not the one it names")
    # Every arm's guides have to arrive on every frame; for the in-memory arms
    # the count is the estimator's own, and for the file arms it is the files'.
    if log["guide_frames"] is not None and log["guide_frames"] != clip["frames"]:
        problems.append(f"guide-frames={log['guide_frames']}, expected {clip['frames']}: "
                        "some frames fell back to the constants")
    if in_memory and log["guide_frames"] is None:
        problems.append("the worker log carries no guide-frames count, so the guides cannot be shown to "
                        "have arrived on every frame")
    if arm == "amd-cpu-nzmt" and log["zero_motion_test"] != "on":
        problems.append("zero-motion-test=off: this arm is defined by that test being on")
    if arm in ("amd-cpu", "amd-cpu-nzmt") and log["zero_motion_test"] != "off" and arm == "amd-cpu":
        problems.append("zero-motion-test=on: arm C is the arm without it")
    if log["history"] in ("off", "absent", "unreadable"):
        problems.append(f"history={log['history']}: the runtime dropped its history, so the temporal "
                        "model never ran; check the guide format and mutual-exclusion checks")
    return problems


def digest(directory: Path) -> str | None:
    mkv = directory / "output.mkv"
    if not mkv.exists():
        return None
    return sequence_digest(framemd5(mkv, directory / "frames.md5"))


def metric(run_dir: Path, key: str):
    path = run_dir / "metrics.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8")).get(key)


def verdict(clips: list[str]) -> list[dict]:
    """The pre-registered decision table, read from analyze.py's metrics.

    Only the two in-memory arms and the zero-motion arm carry a sigma/PSNR
    comparison; the replay arm exists to prove the round trip and is scored by
    digest, not by image metrics.
    """
    rows = []
    for clip in clips:
        c = metric(RUNS / f"{clip}__amd-cpu__1", "temporal_sigma_output")
        cp = metric(RUNS / f"{clip}__amd-cpu-nzmt__1", "temporal_sigma_output")
        z = metric(RUNS / f"{clip}__amd-zmv__1", "temporal_sigma_output")
        cpsnr = metric(RUNS / f"{clip}__amd-cpu__1", "psnr_mean")
        zpsnr = metric(RUNS / f"{clip}__amd-zmv__1", "psnr_mean")
        zflick = metric(RUNS / f"{clip}__amd-zmv__1", "flicker_added")
        cflick = metric(RUNS / f"{clip}__amd-cpu__1", "flicker_added")
        if c is None or z is None:
            continue
        db = (cpsnr - zpsnr) if (cpsnr is not None and zpsnr is not None) else None
        row = dict(clip=clip, sigma_cpu=c, sigma_zmv=z, sigma_nzmt=cp,
                   sigma_delta=(c - z) if (c is not None and z is not None) else None,
                   psnr_cpu=cpsnr, psnr_zmv=zpsnr, psnr_delta_db=db,
                   flicker_cpu=cflick, flicker_zmv=zflick)
        if clip == "still-hold":
            row["reads"] = ("zero-motion test wins" if (cp is not None and cp < c) else
                            "estimator noise is not the problem here" if cp is not None else "n/a")
        else:
            if db is None:
                row["reads"] = "no PSNR"
            elif db > PSNR_TIE_DB:
                row["reads"] = "true motion wins by more than the tie band"
            elif db < -PSNR_TIE_DB:
                row["reads"] = "zero motion wins - keep the constant architecture"
            else:
                row["reads"] = "tie - the motion field buys nothing here"
        rows.append(row)
    return rows


def print_verdict(rows: list[dict]) -> None:
    if not rows:
        print("no metrics yet: run analyze.py first (this script does it unless --skip-analyze)")
        return
    print("\n== decision matrix (pre-registered thresholds) ==")
    print(f"{'clip':16s} {'sigma C':>9s} {'sigma Z':>9s} {'d sigma':>9s} {'PSNR C':>8s} {'PSNR Z':>8s} "
          f"{'d PSNR':>8s}  reads")

    def f(value, digits=4):
        return "n/a" if value is None else f"{value:.{digits}f}"

    for r in rows:
        print(f"{r['clip']:16s} {f(r['sigma_cpu']):>9s} {f(r['sigma_zmv']):>9s} {f(r.get('sigma_delta')):>9s} "
              f"{f(r['psnr_cpu'], 2):>8s} {f(r['psnr_zmv'], 2):>8s} {f(r.get('psnr_delta_db'), 3):>8s}  {r['reads']}")
    moving = [r for r in rows if r["clip"] != "still-hold" and r.get("psnr_delta_db") is not None]
    still = next((r for r in rows if r["clip"] == "still-hold"), None)
    print("\n== overall ==")
    if not moving:
        print("incomplete: no moving-clip comparison")
    else:
        wins = [r for r in moving if r["psnr_delta_db"] > PSNR_TIE_DB]
        losses = [r for r in moving if r["psnr_delta_db"] < -PSNR_TIE_DB]
        if wins and not losses:
            print("true motion wins on every moving clip -> worth taking into the product path")
        elif losses:
            print("zero motion is at least as good on some moving clip -> keep the constant architecture; "
                  "the next lever is FidelityFX Optical Flow, not this field")
        else:
            print("tie on every moving clip -> the motion field buys nothing measurable; "
                  "the runtime's 0.081 blend weight is the ceiling")
    if still is not None:
        # The still clip's own question is arm C against arm C' (the zero-motion
        # test), so its sigma is printed here rather than left to the reader to
        # look up in the analysis report.
        print(f"still-hold: sigma C={f(still['sigma_cpu'])} vs zero-motion-test arm "
              f"{f(still.get('sigma_nzmt'))} -> {still['reads']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*", default=DEFAULT_CLIPS)
    parser.add_argument("--arms", nargs="*", default=ARMS, help="override the dependency order")
    parser.add_argument("--amd-runtime", type=Path, help="the amd-neural-runtime package")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--skip-analyze", action="store_true")
    parser.add_argument("--skip-corpus", action="store_true", help="assume the corpus already exists")
    args = parser.parse_args()

    worker = preflight_worker()
    print(f"worker: {worker}")
    runtime = resolve_amd_runtime(args.amd_runtime)
    print(f"AMD runtime: {runtime}")

    if not args.skip_corpus:
        if not (args.corpus / "manifest.json").exists():
            print("building the corpus (first run only)")
            subprocess.run([sys.executable, str(HERE / "corpus.py"), "--corpus", str(args.corpus)], check=True)
    manifest = load_manifest(args.corpus)
    known = {c["name"] for c in manifest["clips"]}
    unknown = [c for c in args.clips if c not in known]
    if unknown:
        raise SystemExit(f"unknown clips {unknown}; the corpus has {sorted(known)}")

    zero_root = WORK / "guides"
    write_zero_fields(manifest, args.clips, zero_root)

    env = os.environ.copy()
    env["DLSS5_AMD_RUNTIME_DIR"] = str(runtime)
    os.environ.update(env)

    failed, checks, digests = [], [], {}
    by_name = {c["name"]: c for c in manifest["clips"]}
    for arm in args.arms:
        for clip in args.clips:
            directory = RUNS / f"{clip}__{arm}__1"
            print(f"\n---- {clip} / {arm} ----")
            result = run_arm(clip, arm, args.timeout, args.corpus)
            if not result["result"].get("ok"):
                failed.append(f"{clip}/{arm}: {result['result'].get('detail', '')[:200]}")
                continue
            log = parse_log(directory)
            problems = self_check(by_name[clip], arm, log)
            checks.append(dict(clip=clip, arm=arm, log=log, problems=problems))
            for problem in problems:
                print(f"  SELF-CHECK: {problem}")
            digests[(clip, arm)] = digest(directory)

    print("\n== round-trip proof (amd-cpu vs amd-cpu-replay) ==")
    identical = True
    for clip in args.clips:
        a, b = digests.get((clip, "amd-cpu")), digests.get((clip, "amd-cpu-replay"))
        if a is None or b is None:
            print(f"{clip}: incomplete (an arm did not render)")
            identical = False
            continue
        same = a == b
        identical &= same
        print(f"{clip}: {'BYTE-IDENTICAL' if same else 'DIFFERENT'} ({a[:16]} vs {b[:16]})")

    if not args.skip_analyze:
        print("\n== analyze ==")
        subprocess.run([sys.executable, str(HERE / "analyze.py"), "--corpus", str(args.corpus),
                        "--no-ocr", "--no-faces"], check=False)
        print_verdict(verdict(args.clips))

    report = WORK / "amd-guides-ab"
    write_json(report / "selfcheck.json", dict(runtime=str(runtime), clips=args.clips, arms=args.arms,
                                               checks=checks,
                                               digests={f"{c}/{a}": d for (c, a), d in digests.items()},
                                               round_trip_identical=identical, failures=failed))
    print(f"\nself-check: {report / 'selfcheck.json'}")

    if failed:
        print("\nFAILED renders:")
        for line in failed:
            print("  " + line)
    broken = [c for c in checks if c["problems"]]
    if broken:
        print(f"\n{len(broken)} arm(s) failed a self-check; their numbers do not support a conclusion.")
    if failed or broken or not identical:
        return 1
    print("\nall arms rendered, all self-checks passed, round trip byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())