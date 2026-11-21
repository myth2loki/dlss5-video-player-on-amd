#!/usr/bin/env python3
"""
Drive a real DLNA cast session from the player UI and collect objective evidence.

Launches DLSSVideoPlayer on a local clip, clicks the toolbar Cast button, waits
for the async SSDP scan to fill the menu, clicks the first DMR, then verifies
the cast reached the TV using the two signals the product itself exposes:
DLSSVideoPlayer.log lines and the TCP connection from the TV to the in-process
HLS server on port 8080.

The TV picture/audio must still be confirmed by a human in the room - this
script cannot see the TV. Every assertion is a line the shipped code writes:

    Cast: SSDP found ...              src/CastDiscovery.cpp:278
    Cast HTTP: serving ... on ...     src/CastHttpServer.cpp:225
    Cast encoder started: ...         src/CastEncoder.cpp:251
    Cast: streaming to ... via ...    src/CastSession.cpp:253

A missed pattern is a failure with its own reason, never a fast pass.
Run with the WORKSTATION's python (ctypes(user32)) on the interactive desktop.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, subprocess, time, datetime

u = ctypes.windll.user32
WM_CLOSE = 0x0010
LEFTDOWN, LEFTUP = 0x0002, 0x0004

# Window geometry this was measured on (see README in the commit):
# 2560x1440 @ 100% DPI, player moved to (40,40) 2400x1320.
DEFAULT_PLAYER = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
DEFAULT_MEDIA  = r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
DEFAULT_LOG    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.log'
FFMPEG         = r'F:\dlss5-video\dlss5-video-amd\build-ninja\ffmpeg.exe'

WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

def find_player():
    out = []
    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd): return True
        n = u.GetWindowTextLengthW(hwnd)
        if n == 0: return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        if buf.value == 'DLSS 5 Video Player':
            out.append(hwnd)
        return True
    u.EnumWindows(WND_ENUM(cb), 0)
    return out

def click(x, y):
    u.SetCursorPos(x, y)
    time.sleep(0.15)
    u.mouse_event(LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    u.mouse_event(LEFTUP, 0, 0, 0, 0)

def tv_established(tv_ip, local_ip='192.168.1.31', port='8080'):
    """True iff netstat shows an ESTABLISHED socket from the TV to our HLS port."""
    try:
        out = subprocess.run(['netstat', '-ano'], capture_output=True, timeout=15).stdout.decode('utf-8','replace')
    except Exception:
        return False
    # netstat columns: Proto  LocalAddress  ForeignAddress  State
    # TV-initiated HLS request: local = us:8080, foreign = TV:ephemeral,
    # state = ESTABLISHED (or TIME_WAIT after the request completed).
    pat = re.compile(r'^\s*TCP\s+' + re.escape(local_ip) + r':' + port +
                     r'\s+' + re.escape(tv_ip) + r':\d+\s+(ESTABLISHED|TIME_WAIT)', re.I)
    return any(pat.search(l) for l in out.splitlines())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--hold', type=int, default=30)
    ap.add_argument('--tv-ip', default='192.168.1.24')
    ap.add_argument('--shot', default=r'F:\dlss5-video\dlss5-video-amd\cast-active.png')
    a = ap.parse_args()

    if os.path.exists(a.log):
        os.remove(a.log)

    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    samples = []
    try:
        for _ in range(25):
            time.sleep(1)
            wins = find_player()
            if wins:
                hwnd = wins[0]
                break
        if hwnd is None:
            raise RuntimeError('player window never appeared')

        u.MoveWindow(hwnd, 40, 40, 2400, 1320, True)
        time.sleep(0.4)
        u.ShowWindow(hwnd, 9)
        time.sleep(0.2)
        u.SetForegroundWindow(hwnd)
        time.sleep(2)
        print('window:', hex(hwnd), 'fg-match:', u.GetForegroundWindow() == hwnd, flush=True)

        click(a.cast_x, a.cast_y)
        print('clicked Cast; waiting 9s for async SSDP scan + menu refresh', flush=True)
        time.sleep(9)
        click(a.item_x, a.item_y)
        print(f'clicked first device row at ({a.item_x},{a.item_y})', flush=True)

        n = max(2, a.hold // 5)
        for _ in range(n):
            time.sleep(5)
            samples.append(tv_established(a.tv_ip))
        print('tv TCP samples:', ''.join('1' if s else '0' for s in samples), flush=True)

        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab',
                        '-i', 'desktop', '-frames:v', '1', a.shot], timeout=30)
        print('screenshot:', os.path.getsize(a.shot) if os.path.exists(a.shot) else 'NONE', flush=True)
    finally:
        if hwnd is not None:
            u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(1)
        if proc.poll() is None:
            proc.kill()

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []
    print('--- cast evidence lines ---', flush=True)
    for line in lines:
        if 'Cast' in line:
            print('  ', line, flush=True)

    checks = {
        'SSDP discovered':  any('Cast: SSDP found' in l for l in lines),
        'HTTP serving':     any('Cast HTTP: serving' in l for l in lines),
        'encoder started':  any('Cast encoder started' in l for l in lines),
        'streaming to TV':  any('Cast: streaming to' in l for l in lines),
        'no start failure': not any(x in l for l in lines for x in
                                    ('Cast start failed', 'SetAVTransportURI failed', 'Play failed')),
        'TV pulled HLS':    any(samples),
    }
    print('--- verdict ---', flush=True)
    for k, v in checks.items():
        print(f'{k:<20} {"PASS" if v else "FAIL"}', flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)

if __name__ == '__main__':
    main()
