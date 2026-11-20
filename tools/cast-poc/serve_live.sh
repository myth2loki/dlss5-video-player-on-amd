#!/usr/bin/env bash
# R1 stream source: an infinite H.264/AAC HLS live stream + static HTTP server.
#
# The plan's original POC command was video-only and used `-t`-less testsrc2.
# Section 11.2 makes audio mandatory, so this adds an AAC tone: casting a
# silent stream can hide clock-basis differences on some DMRs. Keep both tracks
# from the first POC so the audio path is exercised, not deferred.
#
# Usage:
#   bash tools/cast-poc/serve_live.sh                 # from repo root
#   IP=$(hostname -I | awk '{print $1}')             # Windows: use your LAN IP
#   node tools/cast-poc/cast_sender.js --host <TV_IP> --url http://$IP:8080/live.m3u8
#
# Stop with Ctrl+C. Segments are deleted by ffmpeg (hls_flags delete_segments).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FFMPEG="$REPO/external/ffmpeg/bin/ffmpeg.exe"
OUT="$REPO/tools/cast-poc/live"
PORT="${PORT:-8080}"

if [[ ! -f "$FFMPEG" ]]; then
  echo "ERROR ffmpeg not found: $FFMPEG" >&2
  exit 2
fi
mkdir -p "$OUT"

echo "HLS live dir: $OUT"
echo "Serving on port $PORT (bind 0.0.0.0)"
echo "Stream URL:   http://<THIS_PC_LAN_IP>:$PORT/live.m3u8"
echo "Press Ctrl+C to stop."
echo

# Must be the CORS server, not `python3 -m http.server`: the CAF receiver page
# is served from http://localhost:8011 and fetches this origin cross-origin.
# Without Access-Control-Allow-Origin the browser blocks both the manifest and
# the segments, and CAF reports network/NETWORK_FAILED (fatal).
python3 "$REPO/tools/cast-poc/serve_hls.py" \
  --port "$PORT" --bind 0.0.0.0 --directory "$OUT" &
HTTP_PID=$!
trap 'kill "$HTTP_PID" 2>/dev/null || true' EXIT

# -re feeds at realtime rate; without it testsrc2 encodes as fast as possible
# and the live playlist races ahead of the wall clock. A 12-segment x 2s = 24s
# live window (was 5x2=10s) survives CAF's framework/MediaSource boot delay.
"$FFMPEG" -hide_banner -nostdin \
  -f lavfi -re -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 60 \
  -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_list_size 12 -hls_flags delete_segments \
  -hls_segment_type mpegts -hls_segment_filename "$OUT/seg-%06d.ts" \
  "$OUT/live.m3u8"
