#!/usr/bin/env bash
# Real-footage probe: run all three motion-guide arms on one real clip and
# report how far apart their output frames are.
#
#   run_real_probe.sh <clip.mp4> [outdir]
#
# Why a separate script instead of run_arm.sh: run_arm.sh builds its clip path
# as "$FLOWAB_CLIPS/<name>.mp4", so it cannot take the movie itself or a clip
# outside its clips dir. This one takes an absolute path and keeps the three
# arms apart by directory - a single literal "C:\...\$arm" built with a
# backslash before $arm escapes the dollar instead of separating the path, and
# all three arms then write into one directory called "\$arm" and overwrite
# each other. Paths here always go through cygpath.
#
# Stage-2's synthetic truth does not exist for a movie, so this prints a
# description (which frames, how far apart, on content or on the letterbox
# bars) and writes PNGs. It does not produce a quality verdict.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PLAYER_DIR="${PLAYER_DIR:-$REPO/build-ci/Release}"
CLIP="$1"
OUTBASE="${2:-/c/Temp/dlss5-real}"
NAME="$(basename "$CLIP")"
NAME="${NAME%.*}"
FROM="${FLOWAB_DUMP_FROM:-60}"
COUNT="${FLOWAB_DUMP_COUNT:-12}"
WAIT="${FLOWAB_WAIT:-$((FROM / 24 + 12))}"
CLIP_WIN="$(cygpath -w "$CLIP")"

[ -f "$CLIP" ] || { echo "no such clip: $CLIP" >&2; exit 2; }
mkdir -p "$OUTBASE/runs/$NAME"
cd "$PLAYER_DIR" || exit 1

for arm in ffx cpu off; do
  OUT="$(cygpath -w "$OUTBASE/runs/$NAME/$arm")"
  mkdir -p "$OUTBASE/runs/$NAME/$arm"
  rm -f "$OUTBASE/runs/$NAME/$arm"/out_*.rgba16f "$OUTBASE/runs/$NAME/$arm"/manifest.txt 2>/dev/null
  mv DLSSVideoPlayer.log DLSSVideoPlayer.log.previous 2>/dev/null
  DLSS_VIDEO_MOTION_FLOW="$arm" DLSS5_OUTPUT_DUMP="$OUT" \
  DLSS5_OUTPUT_DUMP_FROM="$FROM" DLSS5_OUTPUT_DUMP_COUNT="$COUNT" \
      ./DLSSVideoPlayer.exe "$CLIP_WIN" >/dev/null 2>&1 &
  sleep "$WAIT"
  taskkill //F //IM DLSSVideoPlayer.exe >/dev/null 2>&1
  sleep 1
  cp DLSSVideoPlayer.log "$OUTBASE/runs/$NAME/$arm/player.log" 2>/dev/null
  frames=$(ls "$OUTBASE/runs/$NAME/$arm"/out_*.rgba16f 2>/dev/null | wc -l)
  backend=$(sed -n 's/.*Motion guide backend: \(.*\)/\1/p' \
            "$OUTBASE/runs/$NAME/$arm/player.log" 2>/dev/null | head -1 | cut -c1-60)
  printf '%-4s frames=%-3s  %s\n' "$arm" "$frames" "$backend"
done

python "$HERE/real_probe_report.py" --dir "$(cygpath -w "$OUTBASE/runs/$NAME")" \
       --png-dir "$(cygpath -w "$OUTBASE/png")"
