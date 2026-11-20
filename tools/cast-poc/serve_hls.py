#!/usr/bin/env python3
"""CORS-enabled static server for the cast-poc HLS live stream.

Why not ``python3 -m http.server``: the CAF receiver page is served from
``http://localhost:8011`` (mattebox receiver proxy) and fetches
``http://<PC_IP>:8080/live.m3u8`` cross-origin. SimpleHTTPRequestHandler emits
no Access-Control-Allow-Origin, so the browser blocks the XHR and CAF reports
``network/NETWORK_FAILED`` (fatal) even though the HLS source itself is healthy.

Also implements byte-range GETs, which some MSE/HLS stacks use to probe
segments. Path traversal and range values are validated before use.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)\Z")
ACCESS_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "access.log")


class HLSHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args) -> None:
        super().log_message(format, *args)
        try:
            with open(ACCESS_LOG, "a", encoding="utf-8") as fh:
                fh.write("%s %s %s\n" % (
                    self.log_date_time_string(), self.address_string(), format % args))
        except OSError:
            pass

    def end_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Range, Content-Type")
        self.send_header(
            "Access-Control-Expose-Headers",
            "Accept-Ranges, Content-Range, Content-Length",
        )
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def do_OPTIONS(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _safe_path(self) -> str | None:
        root = os.path.realpath(self.directory)
        path = os.path.realpath(super().translate_path(self.path))
        # Reserve the server root only: no .., no absolute-path escape, no symlink out.
        if path != root and not path.startswith(root + os.sep):
            self.send_error(403, "Forbidden")
            return None
        return path

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        path = self._safe_path()
        if path is None:
            return
        if os.path.isdir(path) or not os.path.isfile(path):
            return super().do_GET()

        raw_range = self.headers.get("Range")
        if not raw_range:
            return super().do_GET()

        match = RANGE_RE.fullmatch(raw_range.strip())
        if not match or (not match.group(1) and not match.group(2)):
            self.send_error(416, "Invalid Range")
            return

        try:
            with open(path, "rb") as handle:
                size = os.fstat(handle.fileno()).st_size
                first, last = match.group(1), match.group(2)
                if first:
                    start = int(first)
                    end = int(last) if last else size - 1
                else:
                    suffix = int(last)
                    start = max(0, size - suffix)
                    end = size - 1
                if start >= size or start > end:
                    self.send_error(416, "Range Not Satisfiable")
                    return
                end = min(end, size - 1)
                handle.seek(start)
                payload = handle.read(end - start + 1)
        except (OSError, OverflowError, ValueError):
            self.send_error(416, "Range Not Satisfiable")
            return

        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--directory", required=True)
    args = parser.parse_args()

    if not os.path.isdir(args.directory):
        print(f"ERROR directory not found: {args.directory}", file=sys.stderr)
        return 2

    handler = partial(HLSHandler, directory=args.directory)
    server = ThreadingHTTPServer((args.bind, args.port), handler)
    server.daemon_threads = True
    print(f"CORS HLS server: http://{args.bind}:{args.port}/ -> {args.directory}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
