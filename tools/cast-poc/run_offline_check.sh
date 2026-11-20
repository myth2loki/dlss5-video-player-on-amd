#!/usr/bin/env bash
# Offline end-to-end self-check of the R1/R2 tool chain, no TV required.
#
# Two arms through the SAME sender and SAME judge:
#   A healthy   - mock advances playhead 1 s/s  -> judge must say PASS
#   B frozen    - mock freezes at 10 s          -> judge must say FAIL
# If both arms agree, the gate cannot discriminate and the check FAILS.
#
# This validates our wire format and verdict chain, NOT Sony CAF behaviour.
# Real-TV verification is still mandatory (see README.md).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2

OBS_SECONDS="${OBS_SECONDS:-75}"
MOCK_SECONDS=$((OBS_SECONDS + 15))
PORT="${MOCK_PORT:-18009}"

run_arm() {
  local name="$1" mock_arg="$2" trace="$3"
  local mock_pid

  echo "--- arm $name: launching mock on :$PORT $mock_arg ---"
  node mock_receiver.js --port "$PORT" --seconds "$MOCK_SECONDS" $mock_arg > ".mock-$name.log" 2>&1 &
  mock_pid=$!
  # Wait for the listener instead of sleeping a fixed time.
  for _ in $(seq 1 50); do
    if grep -q "MOCK RECEIVER READY" ".mock-$name.log" 2>/dev/null; then break; fi
    sleep 0.2
  done
  if ! grep -q "MOCK RECEIVER READY" ".mock-$name.log" 2>/dev/null; then
    echo "ARM $name FAIL reason=mock_not_ready"; cat ".mock-$name.log"; kill "$mock_pid" 2>/dev/null; return 2
  fi

  node cast_sender.js --host 127.0.0.1 --port "$PORT" \
    --url "http://127.0.0.1:8080/live.m3u8" \
    --seconds "$OBS_SECONDS" --out "$trace"
  local sender_rc=$?
  kill "$mock_pid" 2>/dev/null
  wait "$mock_pid" 2>/dev/null

  if [ "$sender_rc" -ne 0 ]; then
    echo "ARM $name FAIL reason=sender_error rc=$sender_rc"
    return 2
  fi

  python3 judge_r1.py "$trace"
  local judge_rc=$?
  echo "ARM $name judge_rc=$judge_rc"
  return "$judge_rc"
}

echo "=== offline self-check: healthy arm (expect PASS) ==="
run_arm healthy "" trace-healthy.jsonl
healthy_rc=$?

echo "=== offline self-check: frozen arm (expect FAIL) ==="
run_arm frozen "--freeze-after 10" trace-frozen.jsonl
frozen_rc=$?

echo "=== summary ==="
echo "healthy_rc=$healthy_rc (expect 0) frozen_rc=$frozen_rc (expect 1)"
if [ "$healthy_rc" -eq 0 ] && [ "$frozen_rc" -eq 1 ]; then
  echo "OFFLINE-SELFCHECK PASS verdicts_disagree=ok chain=validated"
  exit 0
fi
echo "OFFLINE-SELFCHECK FAIL reason=verdicts_not_discriminating"
exit 1
