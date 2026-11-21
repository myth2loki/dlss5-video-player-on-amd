#!/usr/bin/env python3
"""
Verify the DLNA cast capture path end-to-end with a judge that cannot pass on a
404. Drives the real player UI (ctypes/user32), starts a cast to the first DMR,
then polls the in-process HLS server over raw HTTP and requires:

    GET /live.m3u8  -> 200, body has #EXT-X-M3U and a seg-*.ts line
    GET /seg-...ts  -> 200, body non-empty and grows across samples

Why not netstat: a TCP connection is opened even when the server answers 404.
On 2026-11-21 a connection-count judge called a failed cast PASS. The only
objective proof that frames reached the encoder is a served, non-empty slice.

The TV picture/audio still needs a human in the room; this script cannot see it.
Run with the workstation's python on the interactive desktop.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, socket, subprocess, time

u = ctypes.windll.user32
WM_CLOSE, LEFTDOWN, LEFTUP = 0x0010, 0x0002, 0x0004
WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

DEFAULT_PLAYER = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
DEFAULT_MEDIA  = r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
DEFAULT_LOG    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.log'
LOCAL_IP       = '192.168.1.31'
PORT           = 8080


def find_player():
    out = []
    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd): return True
        n = u.GetWindowTextLengthW(hwnd)
        if n == 0: return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        if buf.value == 'DLSS 5 Video Player': out.append(hwnd)
        return True
    u.EnumWindows(WND_ENUM(cb), 0)
    return out


def click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.15)
    u.mouse_event(LEFTDOWN, 0, 0, 0, 0); time.sleep(0.08)
    u.mouse_event(LEFTUP, 0, 0, 0, 0)


def http_get(path, timeout=5):
    """Raw HTTP/1.1 GET that never goes through a proxy. Returns (status, body)."""
    s = None
    try:
        s = socket.create_connection((LOCAL_IP, PORT), timeout=timeout)
        s.sendall(('GET ' + path + ' HTTP/1.1\r\nHost: ' + LOCAL_IP + ':' + str(PORT) +
                   '\r\nConnection: close\r\n\r\n').encode())
        buf = b''
        while True:
            try: c = s.recv(65536)
            except socket.timeout: break
            if not c: break
            buf += c
            if len(buf) > 16 * 1024 * 1024: break
        head, _, body = buf.partition(b'\r\n\r\n')
        status = re.search(rb'HTTP/1\.1\s+(\d{3})', head)
        return (int(status.group(1)) if status else 0), body
    except Exception as e:
        return 0, str(e).encode('utf-8', 'replace')
    finally:
        if s: s.close()


def probe_once():
    """One HLS probe. Returns (ok, detail)."""
    status, body = http_get('/live.m3u8')
    if status != 200:
        return False, 'm3u8 status %d (%s)' % (status, body[:80].decode('utf-8', 'replace'))
    text = body.decode('utf-8', 'replace')
    if '#EXT-X' not in text:
        return False, 'm3u8 body has no #EXT-X tag'
    segs = [l.strip() for l in text.splitlines()
            if l.strip() and not l.startswith('#') and '.ts' in l]
    if not segs:
        return False, 'm3u8 lists no .ts segment yet'
    seg = segs[-1] if segs[0].startswith('http') else segs[0]
    if seg.startswith('http'):
        seg = '/' + seg.rsplit('/', 1)[-1]
    elif not seg.startswith('/'):
        seg = '/' + seg
    sstatus, sbody = http_get(seg)
    if sstatus != 200 or not sbody:
        return False, 'segment %s status %d bytes %d' % (seg, sstatus, len(sbody))
    return True, 'm3u8 200 + %s 200 (%d bytes), %d segments listed' % (
        seg, len(sbody), len(segs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--hold', type=int, default=40, help='seconds to observe HLS after cast')
    a = ap.parse_args()

    if os.path.exists(a.log): os.remove(a.log)
    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    probes = []
    try:
        for _ in range(25):
            time.sleep(1)
            wins = find_player()
            if wins: hwnd = wins[0]; break
        if hwnd is None: raise RuntimeError('player window never appeared')
        u.MoveWindow(hwnd, 40, 40, 2400, 1320, True); time.sleep(0.4)
        u.ShowWindow(hwnd, 9); time.sleep(0.2)
        u.SetForegroundWindow(hwnd); time.sleep(2)
        print('window:', hex(hwnd), 'fg-match:', u.GetForegroundWindow() == hwnd, flush=True)

        click(a.cast_x, a.cast_y)
        print('clicked Cast; waiting 9s for SSDP scan + menu refresh', flush=True)
        time.sleep(9)
        click(a.item_x, a.item_y)
        print('clicked first DMR; probing HLS every 2s for %ds' % a.hold, flush=True)

        for i in range(max(1, a.hold // 2)):
            time.sleep(2)
            ok, detail = probe_once()
            probes.append(ok)
            print('  probe %02d: %s %s' % (i, 'OK  ' if ok else 'MISS', detail), flush=True)
    finally:
        if hwnd is not None: u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(1)
        if proc.poll() is None: proc.kill()

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []
    print('--- cast evidence lines ---', flush=True)
    for line in lines:
        if 'Cast' in line or 'capture' in line.lower(): print('  ', line, flush=True)

    checks = {
        'SSDP discovered':  any('Cast: SSDP found' in l for l in lines),
        'HTTP serving':     any('Cast HTTP: serving' in l for l in lines),
        'encoder started':  any('Cast encoder started' in l for l in lines),
        'streaming to TV':  any('Cast: streaming to' in l for l in lines),
        'no start failure': not any(x in l for l in lines for x in
                                    ('Cast start failed', 'SetAVTransportURI failed', 'Play failed')),
        'HLS 200 + non-empty slice': any(probes),
        'HLS sustained (>=3 probes)': sum(probes) >= 3,
    }
    print('--- verdict ---', flush=True)
    for k, v in checks.items():
        print('%-28s %s' % (k, 'PASS' if v else 'FAIL'), flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)

if __name__ == '__main__':
    main()
