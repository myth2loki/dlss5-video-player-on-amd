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

    Two ways this harness can run the wrong code, both silent:

    1. common.py's RELEASE_RUNTIME is `<build>/Release/neural-runtime`, where
       `<build>` is DLSS_BENCHMARK_BUILD or `build-upscaling/`. This checkout's
       build script (build-fsr4.bat) builds into `build-ninja/`, so on a fresh
       machine the default points at a directory that does not exist and every
       arm would fail identically with no hint.
    2. run.py's runtime_source() prefers `benchmark-work/runtime-snapshot/` over
       the build whenever that directory holds a NeuralWorker.exe. A snapshot
       left from an earlier session therefore wins over the binary just built -
       and for this plan that means the motion stride fix is NOT in the worker
       whose round-trip proof is supposed to demonstrate it. The proof would
       fail and read as "the fix is wrong" rather than "the snapshot is stale".
       Checked here, before anything renders.
    """
    snapshot = WORK / "runtime-snapshot" / "NeuralWorker.exe"
    worker = RELEASE_RUNTIME / "NeuralWorker.exe"
    if snapshot.exists():
        detail = ""
        if worker.exists():
            detail = (f"\n  snapshot: {snapshot} ({snapshot.stat().st_size} bytes, "
                      f"mtime {snapshot.stat().st_mtime:.0f})\n"
                      f"  build:    {worker} ({worker.stat().st_size} bytes, "
                      f"mtime {worker.stat().st_mtime:.0f})")
        raise SystemExit(
            f"a frozen runtime snapshot exists and would be used instead of the build:{detail}\n"
            f"run.py prefers {snapshot.parent} over {RELEASE_RUNTIME}, so the arms would "
            f"render with that worker, not the one just built - and the round-trip proof that "
            f"has to demonstrate the motion stride fix would be testing the old binary.\n"
            f"Delete or rename that directory, then rerun.")
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


def dependency_problems(clips: list[str], arms: list[str]) -> list[str]:
    """Why this arm list cannot answer anything, before a single frame renders.

    The two failures this catches both surface later as a dead render with a
    "no such file" in it, which reads like a broken file path rather than like a
    run that was assembled wrongly:

    * arm C missing while Z/replay are present - they read its dump and nothing
      in this run produces it;
    * arm C ordered after them - the loop runs the arms in the order given, so
      they would read the dump before it exists.
    """
    problems = []
    needs_c = {"amd-zmv", "amd-cpu-replay"} & set(arms)
    if needs_c and "amd-cpu" not in arms:
        problems.append(f"{sorted(needs_c)} read arm C's guide dump, but amd-cpu is not in --arms; "
                        f"run it first or point those arms at an existing dump")
    elif needs_c:
        first_reader = min(arms.index(a) for a in needs_c)
        if arms.index("amd-cpu") > first_reader:
            problems.append(f"amd-cpu is ordered after {arms[first_reader]}, which reads its dump; "
                            f"the arms run in the order given")
    if "amd-cpu-nzmt" in arms and "amd-cpu" not in arms:
        # C' has to be measured against C to say anything about held frames.
        problems.append("amd-cpu-nzmt is only meaningful beside amd-cpu: it is the same arm "
                        "with the zero-motion test on, and the whole point is the comparison")
    return problems


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
        # Under a `zero/` subdirectory because that is where amd-guides.profile.json
        # reads it: arm Z's guides string is `mv=file:{work}/guides/{clip}/zero/mv`.
        # Writing to `{clip}/mv` instead leaves arm Z pointed at a directory that
        # does not exist, and its every frame fails to read rather than rendering
        # a zero field - a silent-looking failure that would read as "the file
        # path is broken" rather than "the harness wrote to the wrong place".
        for frame in range(clip["frames"]):
            guidefiles.write_frame(root / name / "zero", frame, None, motion)
        written[name] = dict(frames=clip["frames"], grid=f"{gw}x{gh}")
        print(f"zero field: {name} {clip['frames']} frames at {gw}x{gh} -> {root / name / 'zero' / 'mv'}")
    return written


def run_arm(clip: str, arm: str, timeout: float, corpus: Path, fresh: bool) -> dict:
    """Renders one arm through run.py's own machinery, so nothing diverges from
    a profile the harness would run by hand.

    `fresh` matters more here than in a plain ablation run: arm C's dump is arm
    Z's and the replay's input, and run.py skips a run whose result.json already
    exists. A rerun after a profile that names a different dump directory would
    therefore be scored against the previous run's pictures while the directory
    its arm reads is never written - the failure would look like a broken file
    path rather than a stale result.
    """
    import run as runner
    profiles = runner.load_profiles(PROFILE_FILE)
    if arm not in profiles:
        raise SystemExit(f"{PROFILE_FILE} has no arm {arm!r}; it has {list(profiles)}")
    manifest = {c["name"]: c for c in load_manifest(corpus)["clips"]}
    return runner.run_one(manifest[clip], arm, profiles[arm], 1, timeout, corpus, fresh)


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


def parse_log(directory: Path, arm: str | None = None) -> dict:
    """The two log lines that decide whether an arm measured what it claims.

    The bring-up line names the arm (guides, zero-motion test, temporal mode)
    and the per-job line publishes guide-frames, lease-frames and history=.
    Both are the renderer's own account of itself; reading them is cheaper and
    more reliable than inferring the arm from the profile that asked for it.

    Neither line is on the worker's stdout. The renderer writes them to the
    worker's own log, under the per-profile clone run.py makes -
    ``<work>/profiles/<arm>/neural-runtime/NeuralWorker.log`` - while
    ``pass1-stdout.txt`` holds only the runtime's bring-up pulses (arch, driver,
    shader cache). Reading only the stdout therefore found no arm at all and
    every arm failed its self-check with "no bring-up line", which reads like a
    broken render rather than like a log read from the wrong file.

    That log is appended to, one ``===== session started ====`` banner per job,
    and one profile clone serves every clip of an arm - so the job that just ran
    is the LAST session in it. Anything earlier belongs to a previous clip and
    would report another clip's frame count as this one's.
    """
    found = dict(runtime="", guides="", temporal="", zero_motion_test="", guide_frames=None,
                 lease_frames=None, history="")
    sources = [directory / "pass1-stdout.txt", directory / "pass1-stdout-restart.txt"]
    if arm:
        sources.append(WORK / "profiles" / arm / "neural-runtime" / "NeuralWorker.log")
    for path in sources:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        banner = "===== session started"
        start = text.rfind(banner)
        if start >= 0:
            # The newest job. A log that predates this run, or a worker that was
            # reused, would otherwise be read as the arm just rendered.
            text = text[start:]
        for line in reversed(text.splitlines()):
            if not found["runtime"] and "Neural backend:" in line:
                found["runtime"] = _value(line, "runtime", " guides=")
                # `guides=motion:estimator(cpu block matcher) depth:...` - the value
                # contains spaces, so each field ends at the next separator the line
                # actually uses rather than at whitespace. \S+ would cut it at
                # "estimator(cpu" and every check downstream would compare the wrong
                # string. Note `depth:` is a colon in the log, not `depth=`.
                found["guides"] = _value(line, "guides", " depth:")
                found["zero_motion_test"] = _value(line, "zero-motion-test", " temporal=")
                found["temporal"] = "on" if " temporal=on" in line else "off"
            if found["guide_frames"] is None and "AMD neural render:" in line:
                found["guide_frames"] = int(_first(r"guide-frames=(\d+)", line, "0")) or None
                found["lease_frames"] = int(_first(r"lease-frames=(\d+)", line, "0")) or None
                found["history"] = _first(r"history=(\S+)", line)
            if found["runtime"] and found["guide_frames"] is not None:
                break
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
    if arm == "amd-cpu" and log["zero_motion_test"] == "on":
        # C is defined as the arm WITHOUT the test; C' is the one with it. If C
        # renders with it on, the two arms are the same configuration and their
        # difference measures nothing.
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

    Two different questions with two different pairs behind them:

    * still-hold asks C against C' - does zeroing the field on held frames buy
      anything - and needs neither arm Z nor a PSNR comparison.
    * the moving clips ask C against Z - does a true field beat a zero one - and
      need both.

    So a run of only C and C' still produces its row, and a run missing Z drops
    only the rows it cannot answer rather than the whole table. The replay arm
    is absent from this function on purpose: it is scored by frame digest in the
    round-trip proof, not by image metrics.
    """
    rows = []
    for clip in clips:
        c = metric(RUNS / f"{clip}__amd-cpu__1", "temporal_sigma_output")
        cp = metric(RUNS / f"{clip}__amd-cpu-nzmt__1", "temporal_sigma_output")
        z = metric(RUNS / f"{clip}__amd-zmv__1", "temporal_sigma_output")
        cpsnr = metric(RUNS / f"{clip}__amd-cpu__1", "psnr_mean")
        zpsnr = metric(RUNS / f"{clip}__amd-zmv__1", "psnr_mean")
        cflick = metric(RUNS / f"{clip}__amd-cpu__1", "flicker_added")
        zflick = metric(RUNS / f"{clip}__amd-zmv__1", "flicker_added")
        db = (cpsnr - zpsnr) if (cpsnr is not None and zpsnr is not None) else None
        if clip == "still-hold":
            if c is None and cp is None:
                continue
            if c is None:
                reads = f"only arm C' rendered (sigma {cp:.4f}); no comparison"
            elif cp is None:
                reads = "arm C' did not render, so the held-frame question is unanswered"
            elif cp < c:
                reads = "zero-motion test wins on held frames - worth keeping (matches the NVIDIA mv-gate)"
            elif cp == c:
                reads = "identical: the estimator invented nothing on held frames, so the test has nothing to fix"
            else:
                reads = "the test made held frames worse - do not take it"
            rows.append(dict(clip=clip, sigma_cpu=c, sigma_zmv=z, sigma_nzmt=cp,
                             sigma_delta=(c - z) if (c is not None and z is not None) else None,
                             psnr_cpu=cpsnr, psnr_zmv=zpsnr, psnr_delta_db=db,
                             flicker_cpu=cflick, flicker_zmv=zflick, reads=reads))
            continue
        if c is None or z is None:
            continue
        if db is None:
            reads = "no PSNR"
        elif db > PSNR_TIE_DB:
            reads = "true motion wins by more than the tie band"
        elif db < -PSNR_TIE_DB:
            reads = "zero motion wins - keep the constant architecture"
        else:
            reads = "tie - the motion field buys nothing here"
        rows.append(dict(clip=clip, sigma_cpu=c, sigma_zmv=z, sigma_nzmt=cp,
                         sigma_delta=c - z, psnr_cpu=cpsnr, psnr_zmv=zpsnr,
                         psnr_delta_db=db, flicker_cpu=cflick, flicker_zmv=zflick, reads=reads))
    return rows


def print_verdict(rows: list[dict]) -> None:
    if not rows:
        print("no metrics yet: run analyze.py first (this script does it unless --skip-analyze)")
        return
    print("\n== decision matrix (pre-registered thresholds) ==")
    print(f"{'clip':16s} {'sigma C':>9s} {'sigma C-prime':>13s} {'sigma Z':>9s} {'d sigma C-Z':>11s} "
          f"{'PSNR C':>8s} {'PSNR Z':>8s} {'d PSNR':>8s}  reads")

    def f(value, digits=4):
        return "n/a" if value is None else f"{value:.{digits}f}"

    for r in rows:
        print(f"{r['clip']:16s} {f(r['sigma_cpu']):>9s} {f(r.get('sigma_nzmt')):>13s} "
              f"{f(r['sigma_zmv']):>9s} {f(r.get('sigma_delta')):>11s} "
              f"{f(r['psnr_cpu'], 2):>8s} {f(r['psnr_zmv'], 2):>8s} "
              f"{f(r.get('psnr_delta_db'), 3):>8s}  {r['reads']}")
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
        # Repeated here rather than only in the matrix because it answers a
        # different question from the moving clips, and "overall" is where a
        # reader looks for a verdict. One line, and it carries the numbers.
        print(f"still-hold (held-frame noise): sigma C={f(still['sigma_cpu'])} vs C'={f(still.get('sigma_nzmt'))} "
              f"-> {still['reads']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*", default=DEFAULT_CLIPS)
    parser.add_argument("--arms", nargs="*", default=ARMS, help="override the dependency order")
    parser.add_argument("--amd-runtime", type=Path, help="the amd-neural-runtime package")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--skip-analyze", action="store_true")
    parser.add_argument("--fresh", action="store_true",
                        help="rebuild each profile's runtime clone and re-render arms whose result.json "
                             "already exists (default: reuse, so a resumed run does not redo hours of work)")
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

    # Checked before anything is written, so a run that cannot answer its
    # question fails without leaving a half-prepared tree behind. Two facts the
    # profile cannot express on its own, both of which otherwise surface as a
    # dead render that reads like a broken path:
    #
    #  * arms Z and the replay READ arm C's dump, so arm C must be present and
    #    ordered before them (the arms run in the order given);
    #  * C' is only meaningful beside C - it is the same arm with the test on.
    problems = dependency_problems(args.clips, args.arms)
    if problems:
        raise SystemExit("the run cannot answer its question:\n  " + "\n  ".join(problems))

    write_zero_fields(manifest, args.clips, zero_root)

    env = os.environ.copy()
    env["DLSS5_AMD_RUNTIME_DIR"] = str(runtime)
    os.environ.update(env)

    failed, checks, digests = [], [], {}
    by_name = {c["name"]: c for c in manifest["clips"]}
    # Which clips arm C actually dumped for. A clip whose C arm failed leaves no
    # fresh dump - and possibly the PREVIOUS run's dump in that directory, since
    # nothing clears it. Z and the replay would then render happily from stale
    # guides and their numbers would look like a result. Tracked per clip and
    # used to skip those arms rather than to score them.
    dumped: set[str] = set()
    for arm in args.arms:
        for clip in args.clips:
            if arm in ("amd-zmv", "amd-cpu-replay") and clip not in dumped:
                print(f"\n---- {clip} / {arm}: skipped, arm C produced no dump for this clip ----")
                failed.append(f"{clip}/{arm}: skipped because amd-cpu did not produce a dump")
                continue
            directory = RUNS / f"{clip}__{arm}__1"
            print(f"\n---- {clip} / {arm} ----")
            result = run_arm(clip, arm, args.timeout, args.corpus, args.fresh)
            if not result["result"].get("ok"):
                failed.append(f"{clip}/{arm}: {result['result'].get('detail', '')[:200]}")
                continue
            log = parse_log(directory, arm)
            problems = self_check(by_name[clip], arm, log)
            checks.append(dict(clip=clip, arm=arm, log=log, problems=problems))
            for problem in problems:
                print(f"  SELF-CHECK: {problem}")
            # A dump is only trustworthy when the arm that wrote it passed its
            # own checks: a C arm whose guides never arrived wrote something,
            # but not the field this comparison is about.
            if arm == "amd-cpu" and not problems:
                dumped.add(clip)
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