#!/usr/bin/env bash
# Stage-2 batch: dump FSR 4 output frames for every shot x arm.
#
#   run_output_ab.sh                 # all shots in truth.json, arms ffx/cpu/off
#   FLOWAB_SHOTS="o_pan2 o_real_a" FLOWAB_ARMS="ffx cpu" run_output_ab.sh
#
# Paths follow gen_truth.py's --out. Each run is one fresh player process, so
# the arms never share a temporal history (a warm FSR 4 context would leak one
# arm's reprojection into the next one's first frames).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${FLOWAB_OUTPUTS:-$HERE/outputs}"
RUNS="${FLOWAB_RUNS:-$OUT/runs}"
CLIPS="${FLOWAB_CLIPS:-$OUT/clips}"
ARMS="${FLOWAB_ARMS:-ffx cpu off}"
# Every pass is a fresh set of player processes. Pass 1 is the scored batch,
# pass 2 measures the same-configuration noise floor, pass 3 (optional) lets
# summarize_output.py take a per-frame median over three runs.
PASSES="${FLOWAB_PASSES:-2}"
META="$OUT/truth.json"
# The Python on this host is a native win32 build: a POSIX path such as
# /c/Temp/... is resolved relative to the CURRENT drive (F:\c\Temp\...), not to
# C:. Convert the manifest path before handing it to Python. The shell itself
# keeps using the POSIX form, so mkdir / cygpath in run_arm.sh still work.
META_WIN="$(cygpath -w "$META")"
FROM="${FLOWAB_DUMP_FROM:-$(python -c 'import json,sys; print(json.load(open(sys.argv[1],encoding="utf-8"))["dump_from"])' "$META_WIN")}"
COUNT="${FLOWAB_DUMP_COUNT:-$(python -c 'import json,sys; print(json.load(open(sys.argv[1],encoding="utf-8"))["dump_count"])' "$META_WIN")}"
if [ -z "${FLOWAB_SHOTS:-}" ]; then
  SHOTS="$(python -c 'import json,sys; print(" ".join(s["name"] for s in json.load(open(sys.argv[1],encoding="utf-8"))["shots"]))' "$META_WIN")"
else
  SHOTS="$FLOWAB_SHOTS"
fi
echo "stage-2 batch: from=$FROM count=$COUNT passes=$PASSES arms=$ARMS"
for pass in $(seq 1 "$PASSES"); do
  case "$pass" in
    1) PASS_RUNS="$RUNS" ;;
    2) PASS_RUNS="${FLOWAB_REPEAT_RUNS:-$OUT/runs_repeat}" ;;
    *) PASS_RUNS="$OUT/runs_r$pass" ;;
  esac
  for shot in $SHOTS; do
    for arm in $ARMS; do
      FLOWAB_CLIPS="$CLIPS" FLOWAB_RUNS="$PASS_RUNS" \
      FLOWAB_DUMP_FROM="$FROM" FLOWAB_DUMP_COUNT="$COUNT" \
        bash "$HERE/run_arm.sh" "$shot" "$arm" output || exit 1
    done
  done
done
