#!/usr/bin/env bash
# One arm of the motion-guide A/B: play a clip, dump the field the upscaler was
# given, and keep the log line that names which estimator produced it.
#
#   run_arm.sh <clip-name> <cpu|ffx|off> [field|output|both]
#
# `field` (default) is the stage-1 motion-field dump. `output` is the stage-2
# FSR 4 output-frame dump; `both` writes the two side by side with the same
# FROM/COUNT and the same source-frame index, so they pair up by filename.
#
# No ini is touched. The first attempt drove the control arm through the ini's
# [NeuralGuides] MotionVectors key and the player rewrote that key from its own
# state, so the arm silently ran as the CPU arm - its dump came out byte for byte
# identical to cpu's, which is how it was caught.
set -u
# Paths are relative to this script, not to the machine the run was first made on.
# Override PLAYER_DIR / FLOWAB_RUNS / FLOWAB_CLIPS to point at another checkout.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PLAYER_DIR="${PLAYER_DIR:-$REPO/build-ci/Release}"
ROOT="${FLOWAB_RUNS:-$HERE/runs}"
CLIPS="${FLOWAB_CLIPS:-$HERE/clips}"
TRASH="${FLOWAB_TRASH:-$HERE/trash}"
CLIP_WIN="$(cygpath -w "$CLIPS")"
NAME="$1"; ARM="$2"; MODE="${3:-field}"
# Stage 2 dumps after warm-up (frame 36); run_output_ab.sh reads these from
# truth.json. Stage-1 field reproduction can override with 12/12.
FROM="${FLOWAB_DUMP_FROM:-36}"
COUNT="${FLOWAB_DUMP_COUNT:-12}"
case "$MODE" in
  field|output|both) ;;
  *) echo "usage: run_arm.sh <clip-name> <cpu|ffx|off> [field|output|both]" >&2; exit 2 ;;
esac
OUT="$ROOT/$NAME/$ARM"
OUT_WIN="$(cygpath -w "$OUT")"
# Keep the separator in its own variable: a backslash immediately before $NAME
# inside double quotes escapes the dollar instead of separating the path.
SEP='\'
CLIP_ARG="${CLIP_WIN}${SEP}${NAME}.mp4"

mkdir -p "$TRASH"
mv "$OUT" "$TRASH/$(basename "$(dirname "$OUT")")-$(basename "$OUT")-$(date +%s%N)" 2>/dev/null
mkdir -p "$OUT"

cd "$PLAYER_DIR" || exit 1
mv DLSSVideoPlayer.log DLSSVideoPlayer.log.previous 2>/dev/null
# Assignment prefixes, not `env`: an MSYS `env` starting a Windows binary here
# produced a process that exited before its first log line. No `eval` either -
# a second expansion pass mangles the backslashes in the Windows clip path.
case "$MODE" in
  field)
    DLSS_VIDEO_MOTION_FLOW="$ARM" \
    DLSS5_FLOW_DUMP="$OUT_WIN" \
    DLSS5_FLOW_DUMP_FROM="$FROM" \
    DLSS5_FLOW_DUMP_COUNT="$COUNT" \
        ./DLSSVideoPlayer.exe "$CLIP_ARG" >/dev/null 2>&1 & ;;
  output)
    DLSS_VIDEO_MOTION_FLOW="$ARM" \
    DLSS5_OUTPUT_DUMP="$OUT_WIN" \
    DLSS5_OUTPUT_DUMP_FROM="$FROM" \
    DLSS5_OUTPUT_DUMP_COUNT="$COUNT" \
        ./DLSSVideoPlayer.exe "$CLIP_ARG" >/dev/null 2>&1 & ;;
  both)
    DLSS_VIDEO_MOTION_FLOW="$ARM" \
    DLSS5_FLOW_DUMP="$OUT_WIN" \
    DLSS5_FLOW_DUMP_FROM="$FROM" \
    DLSS5_FLOW_DUMP_COUNT="$COUNT" \
    DLSS5_OUTPUT_DUMP="$OUT_WIN" \
    DLSS5_OUTPUT_DUMP_FROM="$FROM" \
    DLSS5_OUTPUT_DUMP_COUNT="$COUNT" \
        ./DLSSVideoPlayer.exe "$CLIP_ARG" >/dev/null 2>&1 & ;;
esac
sleep 14
taskkill //F //IM DLSSVideoPlayer.exe >/dev/null 2>&1
sleep 1

# A log older than this run means the copy below would describe the previous
# arm; say so instead of reporting its backend as this arm's.
if [ -n "$(find DLSSVideoPlayer.log -newermt '-60 seconds' 2>/dev/null)" ]; then
  cp DLSSVideoPlayer.log "$OUT/player.log"
else
  echo "STALE LOG" > "$OUT/player.log"
fi
case "$MODE" in
  field) PATTERN='flow_*.rg16f' ;;
  *)     PATTERN='out_*.rgba16f' ;;
esac
FRAMES=$(ls "$OUT"/$PATTERN 2>/dev/null | wc -l)
BACKEND=$(sed -n 's/.*Motion guide backend: \(.*\)/\1/p' "$OUT/player.log" 2>/dev/null | head -1)
DROPS=$(grep -o 'dropped=[0-9]* (total [0-9]*)' "$OUT/player.log" 2>/dev/null | tail -1)
printf '%-8s %-4s frames=%-3s %s\n    %s\n' "$NAME" "$ARM" "$FRAMES" "$DROPS" "$BACKEND"
