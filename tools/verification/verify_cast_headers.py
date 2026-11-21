"""Check the HTTP headers a DLNA DMR actually receives from the product player.

Background: the TV pulled every HLS request (200/206, TCP established) yet
showed "current content not playable". The Python POC that DID play served
.m3u8 as application/vnd.apple.mpegurl and .ts as video/mp2t, while the
product CastHttpServer served everything as application/octet-stream. This
driver casts from the real player and reads the headers back off port 8080.

Must see (judge lines):
  M3U8_CONTENT_TYPE application/vnd.apple.mpegurl
  SEG_CONTENT_TYPE  video/mp2t

Run from the repo root with no player already running. It leaves no process
behind. GUI injection uses ctypes (PowerShell Add-Type is blocked here).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import time
import urllib.request

u = ctypes.windll.user32
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def click(x, y):
    u.SetCursorPos(x, y)
    time.sleep(0.15)
    u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def find_player():
    out = []

    def cb(hwnd, _):
        if not u.IsWindowVisible(hwnd):
            return True
        n = u.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        if buf.value == 'DLSS 5 Video Player':
            out.append(hwnd)
        return True

    u.EnumWindows(WND_ENUM(cb), 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe')
    ap.add_argument('--media', default=r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv')
    ap.add_argument('--port', default='8080')
    ap.add_argument('--scan-wait', type=int, default=10)
    args = ap.parse_args()

    p = subprocess.Popen([args.player, args.media], cwd=os.path.dirname(args.player))
    hwnd = None
    try:
        for _ in range(25):
            time.sleep(1)
            found = find_player()
            if found:
                hwnd = found[0]
                break
        if hwnd is None:
            print('JUDGE  FAIL  reason=no_player_window')
            return 1
        # The toolbar coordinates below are valid only for this exact rect.
        u.MoveWindow(hwnd, 40, 40, 2400, 1320, True)
        time.sleep(0.4)
        u.ShowWindow(hwnd, 9)
        time.sleep(0.2)
        u.SetForegroundWindow(hwnd)
        time.sleep(0.6)

        click(490, 1266)             # Cast toolbar button
        time.sleep(args.scan_wait)   # async SSDP scan + menu auto-reopen
        click(570, 1144)             # first selectable device (SMG投屏24)

        base = 'http://192.168.1.31:' + args.port + '/'
        m3u8 = None
        for _ in range(20):          # first segment needs up to hls_time=2s
            try:
                m3u8 = urllib.request.urlopen(base + 'live.m3u8', timeout=5)
                break
            except Exception:
                time.sleep(1)
        if m3u8 is None:
            print('JUDGE  FAIL  reason=m3u8_never_served')
            return 1
        print('M3U8_STATUS', m3u8.status)
        print('M3U8_CONTENT_TYPE', m3u8.headers.get('Content-Type'))
        body = m3u8.read().decode('utf-8', 'replace')
        seg = next((line.strip() for line in body.splitlines()
                    if line.strip() and not line.startswith('#')), None)
        print('FIRST_SEGMENT', seg)
        if seg:
            r2 = urllib.request.urlopen(base + seg, timeout=8)
            print('SEG_STATUS', r2.status)
            print('SEG_CONTENT_TYPE', r2.headers.get('Content-Type'))
            r2.read(1024)
        ct_m3u8 = m3u8.headers.get('Content-Type', '')
        ct_seg = ''
        if seg:
            ct_seg = urllib.request.urlopen(base + seg, timeout=8).headers.get('Content-Type', '')
        ok = ct_m3u8 == 'application/vnd.apple.mpegurl' and ct_seg == 'video/mp2t'
        print('JUDGE', 'PASS' if ok else 'FAIL', 'm3u8=' + ct_m3u8, 'seg=' + ct_seg)
        return 0 if ok else 1
    finally:
        if hwnd is not None:
            u.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
            time.sleep(1)
        if p.poll() is None:
            p.kill()


if __name__ == '__main__':
    raise SystemExit(main())
