"""R1 judge: turn Cast MEDIA_STATUS traces into a falsifiable PASS/FAIL.

Implements plan section 11.6 (hard thresholds replace "looks fine"):
  * first frame <= 12 s after LOAD
  * >= 60 s of observation window after the first frame
  * playhead net advance >= 45 s within that window (headroom for buffering)
  * no ERROR status, no stalled playhead longer than 3 s

Input is JSONL, one object per line, produced by cast_sender.js (or pasted
from the mattebox Messages panel). Recognised events:

  {"t_ms": 0,    "event": "load"}
  {"t_ms": 4300, "event": "status", "playerState": "PLAYING", "currentTime": 0.0}
  {"t_ms": 8000, "event": "error",  "reason": "LOAD_FAILED"}

Usage:
    python3 tools/cast-poc/judge_r1.py trace.jsonl
    python3 tools/cast-poc/judge_r1.py --selftest

Exit code is the verdict: 0 PASS, 1 FAIL, 2 malformed input.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

FIRST_FRAME_MAX_MS = 12_000
WINDOW_S = 60.0
MIN_ADVANCE_S = 45.0
MAX_STALL_S = 3.0
LIVE_WRAP_S = 30.0  # a negative jump this large is a live-window rebase, not a stall

ERROR_STATES = {"IDLE"}
ERROR_REASONS = {"ERROR", "LOAD_FAILED", "MEDIA_ERROR"}


@dataclass
class Sample:
    t_ms: int
    state: str
    current_time: float | None


def read_trace(path: Path) -> tuple[int | None, list[Sample], list[str]]:
    load_at: int | None = None
    samples: list[Sample] = []
    errors: list[str] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"line {lineno}: bad JSON: {exc}") from exc
        t = row.get("t_ms")
        if not isinstance(t, (int, float)) or t < 0:
            raise ValueError(f"line {lineno}: missing/invalid t_ms")
        event = row.get("event")
        if event == "load":
            if load_at is None:
                load_at = int(t)
        elif event == "status":
            ct = row.get("currentTime")
            samples.append(
                Sample(int(t), str(row.get("playerState", "")), float(ct) if isinstance(ct, (int, float)) else None)
            )
            state = str(row.get("state", ""))
            reason = str(row.get("reason", ""))
            idle_reason = str(row.get("idleReason", ""))
            status_result = str(row.get("statusResult", ""))
            if (state in ERROR_STATES and reason in ERROR_REASONS) or reason in ERROR_REASONS \
                    or idle_reason in ERROR_REASONS or status_result in ERROR_REASONS:
                errors.append(f"status@{int(t)}:{reason or idle_reason or status_result}")
        elif event == "error":
            errors.append(f"error@{int(t)}:{row.get('reason', 'unknown')}")
    return load_at, samples, errors


def judge(load_at: int | None, samples: list[Sample], errors: list[str]) -> tuple[str, str]:
    if load_at is None:
        return "FAIL", "reason=no_load_event"
    if not samples:
        return "FAIL", "reason=no_media_status"

    # First frame = first PLAYING status with a moving playhead.
    first = next(
        (s for s in samples if s.state == "PLAYING" and s.current_time is not None and s.current_time > 0.0),
        None,
    )
    if first is None:
        first_playing = next((s for s in samples if s.state == "PLAYING"), None)
        if first_playing is None:
            return "FAIL", "reason=never_playing"
        first_ms = first_playing.t_ms - load_at
        return "FAIL", f"reason=playhead_never_advanced first_status_ms={first_ms}"

    first_ms = first.t_ms - load_at
    if first_ms > FIRST_FRAME_MAX_MS:
        return "FAIL", f"reason=first_frame_too_slow first_frame_ms={first_ms} limit_ms={FIRST_FRAME_MAX_MS}"

    window_end = first.t_ms + int(WINDOW_S * 1000)
    window = [s for s in samples if first.t_ms <= s.t_ms <= window_end]
    window_ct = [s for s in window if s.current_time is not None]
    if len(window_ct) < 2:
        return "FAIL", f"reason=too_few_status_samples n={len(window_ct)} window_s={WINDOW_S:.0f}"

    # Live windows rebase the playhead; a large negative jump is not a stall.
    base = window_ct[0].current_time
    net = 0.0
    wraps = 0
    prev_t = window_ct[0].t_ms
    prev_ct = window_ct[0].current_time
    max_stall_s = 0.0
    stall_start: float | None = None
    for s in window_ct[1:]:
        delta = s.current_time - prev_ct
        if delta < -LIVE_WRAP_S:
            wraps += 1
            base = s.current_time
            prev_t, prev_ct = s.t_ms, s.current_time
            stall_start = None
            continue
        if delta <= 0.01:
            if stall_start is None:
                stall_start = prev_t / 1000.0
            max_stall_s = max(max_stall_s, s.t_ms / 1000.0 - stall_start)
        else:
            stall_start = None
        net = max(net, s.current_time - base)
        prev_t, prev_ct = s.t_ms, s.current_time

    observed_s = (window_ct[-1].t_ms - window_ct[0].t_ms) / 1000.0
    if observed_s < WINDOW_S - 5.0:
        return "FAIL", f"reason=window_too_short observed_s={observed_s:.1f} required_s={WINDOW_S - 5:.0f}"
    if net < MIN_ADVANCE_S:
        return "FAIL", f"reason=playhead_not_advancing net_s={net:.1f} required_s={MIN_ADVANCE_S:.0f}"
    if max_stall_s > MAX_STALL_S:
        return "FAIL", f"reason=playhead_stalled max_stall_s={max_stall_s:.1f} limit_s={MAX_STALL_S:.0f}"
    if errors:
        return "FAIL", f"reason=error_status errors={';'.join(errors[:3])}"

    return (
        "PASS",
        f"first_frame_ms={first_ms} window_s={observed_s:.1f} net_advance_s={net:.1f} "
        f"max_stall_s={max_stall_s:.1f} samples={len(window_ct)} live_wraps={wraps}",
    )


def selftest() -> int:
    """A gate that cannot fail is useless. Prove both verdicts from fixed traces."""
    import io
    import tempfile

    def trace(lines: str) -> Path:
        fd, name = tempfile.mkstemp(suffix=".jsonl")
        Path(name).write_text(lines.lstrip(), encoding="utf-8")
        import os
        os.close(fd)
        return Path(name)

    good = io.StringIO()
    good.write('{"t_ms":0,"event":"load"}\n')
    good.write('{"t_ms":3500,"event":"status","playerState":"BUFFERING","currentTime":0}\n')
    for i in range(66):
        t = 5000 + i * 1000
        good.write(f'{{"t_ms":{t},"event":"status","playerState":"PLAYING","currentTime":{i + 1}}}\n')
    good_path = trace(good.getvalue())

    bad = io.StringIO()
    bad.write('{"t_ms":0,"event":"load"}\n')
    for i in range(66):
        t = 5000 + i * 1000
        # Frozen playhead after 10 s: the "looks fine" case this gate must catch.
        ct = min(i + 1, 10)
        bad.write(f'{{"t_ms":{t},"event":"status","playerState":"PLAYING","currentTime":{ct}}}\n')
    bad_path = trace(bad.getvalue())

    good_result = judge(*read_trace(good_path))
    bad_result = judge(*read_trace(bad_path))
    good_path.unlink()
    bad_path.unlink()

    print(f"SELFTEST good -> {good_result[0]} {good_result[1]}")
    print(f"SELFTEST bad  -> {bad_result[0]} {bad_result[1]}")
    if good_result[0] == "PASS" and bad_result[0] == "FAIL":
        print("SELFTEST PASS verdicts_disagree=ok")
        return 0
    print("SELFTEST FAIL verdicts_not_discriminating")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", nargs="?", type=Path, help="JSONL trace from cast_sender.js")
    parser.add_argument("--selftest", action="store_true", help="verify the gate distinguishes pass/fail")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.trace is None:
        parser.error("give a trace file or --selftest")
    try:
        verdict, detail = judge(*read_trace(args.trace))
    except (OSError, ValueError) as exc:
        print(f"POC-R1 JUDGE FAIL reason=bad_trace detail={exc}")
        return 2
    print(f"POC-R1 JUDGE {verdict} {detail}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
