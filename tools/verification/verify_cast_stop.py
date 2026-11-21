#!/usr/bin/env python3
"""Verify the UI Stop path end-to-end against the real TV.

Drives the shipped player UI: start a cast to the first DMR, confirm the TV is
actually PLAYING via SOAP GetTransportInfo, then reopen the Cast menu and click
the first row (IDM_CAST_STOP, appended when Casting()).  The judge requires the
TV's own DMR state to become STOPPED and the local HLS port to stop answering.

Why SOAP state, not the log: CastSession::Stop() ignores the SOAP Stop return
value, so no log line proves the DMR accepted it.  Only GetTransportInfo does.

Judge lines (all must appear):
  CASTING_STATE PLAYING        before clicking Stop
  STOP_ITEM <x>,<y>            the menu row we actually clicked
  STOPPED_STATE STOPPED        after clicking Stop (TV DMR reported)
  HLS_PORT closed              local GET /live.m3u8 no longer 200
Toast "cast.stopped" in the log is an extra, not a substitute.
Run with the workstation python on the interactive desktop.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
POC = os.path.normpath(os.path.join(HERE, '..', 'dlna-poc'))
sys.path.insert(0, POC)
import dlna_sender as d  # noqa: E402

u = ctypes.windll.user32
WM_CLOSE, LEFTDOWN, LEFTUP = 0x0010, 0x0002, 0x0004
MN_GETHMENU = 0x01E1
WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

DEFAULT_PLAYER = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
DEFAULT_MEDIA  = r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
DEFAULT_LOG    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.log'
LOCAL_IP, PORT = '192.168.1.31', 8080
FFMPEG = r'F:\dlss5-video\dlss5-video-amd\build-ninja\ffmpeg.exe'

# 64-bit-safe ctypes signatures (handles are 64-bit on this workstation).
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetWindowRect.restype = wt.BOOL
u.SendMessageW.restype = wt.LPARAM
u.GetMenuItemRect.argtypes = [wt.HWND, ctypes.c_void_p, wt.UINT, ctypes.POINTER(wt.RECT)]
u.GetMenuItemRect.restype = wt.BOOL


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


def find_menu_hwnd():
    out = []
    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd): return True
        buf = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(hwnd, buf, 64)
        if buf.value == '#32768': out.append(hwnd)
        return True
    u.EnumWindows(WND_ENUM(cb), 0)
    return out


def click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.15)
    u.mouse_event(LEFTDOWN, 0, 0, 0, 0); time.sleep(0.08)
    u.mouse_event(LEFTUP, 0, 0, 0, 0)


def http_status(path, timeout=4):
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
            if len(buf) > 8 * 1024 * 1024: break
        head = buf.split(b'\r\n\r\n', 1)[0]
        m = re.search(rb'HTTP/1\.1\s+(\d{3})', head)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0
    finally:
        if s: s.close()


def tv_state(control):
    try:
        st, info = d.soap_call(control, d.AVTRANSPORT, 'GetTransportInfo', {'InstanceID': '0'})
        return d.parse_tag(info, 'CurrentTransportState') or '?'
    except Exception:
        return '?'


def find_dmr():
    for loc in d.ssdp_discover(LOCAL_IP, 6):
        try:
            friendly, base, ctl = d.parse_description(loc)
        except Exception:
            continue
        if any(k in friendly for k in ('CAST', '投屏', 'DMR')):
            return friendly, ctl
    return None, None


def first_menu_item_center():
    """Locate the first popup-menu row via the Win32 menu API, no guessed coords."""
    menus = find_menu_hwnd()
    if not menus:
        return None, 'no #32768 menu window'
    hwnd = menus[0]
    mr = wt.RECT(); u.GetWindowRect(hwnd, ctypes.byref(mr))
    detail = 'menu_rect=(%d,%d,%d,%d)' % (mr.left, mr.top, mr.right, mr.bottom)
    hMenu = u.SendMessageW(hwnd, MN_GETHMENU, 0, 0)
    if hMenu:
        ir = wt.RECT()
        if u.GetMenuItemRect(0, ctypes.c_void_p(hMenu), 0, ctypes.byref(ir)):
            cx, cy = (ir.left + ir.right) // 2, (ir.top + ir.bottom) // 2
            return (cx, cy), detail + ' item0=(%d,%d,%d,%d)' % (ir.left, ir.top, ir.right, ir.bottom)
    # Fallback: first row sits just under the menu window top.
    return ((mr.left + mr.right) // 2, mr.top + 14), detail + ' (fallback top+14)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--tv', default='192.168.1.24')
    ap.add_argument('--shot', default=r'F:\dlss5-video\dlss5-video-amd\cast-stopped.png')
    a = ap.parse_args()

    if os.path.exists(a.log): os.remove(a.log)
    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    friendly, control = None, None
    before = after = '?'
    stop_item = None
    item_detail = ''
    hls_during = hls_after = 0
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

        # Start the cast exactly as the capture verifier does.
        click(a.cast_x, a.cast_y); time.sleep(9)
        click(a.item_x, a.item_y)
        print('clicked first DMR; waiting 6s for HLS + TV to settle', flush=True)
        time.sleep(6)
        hls_during = http_status('/live.m3u8')
        friendly, control = find_dmr()
        print('DMR=%s control=%s' % (friendly, control), flush=True)
        if not control: raise RuntimeError('no DMR found for SOAP state check')
        before = tv_state(control)
        print('CASTING_STATE %s (hls=%d)' % (before, hls_during), flush=True)

        # Reopen the Cast menu while casting; first row is now IDM_CAST_STOP.
        click(a.cast_x, a.cast_y); time.sleep(1.0)
        stop_item, item_detail = first_menu_item_center()
        print('STOP_ITEM %s %s' % (
            ('%d,%d' % stop_item) if stop_item else 'NONE', item_detail), flush=True)
        if stop_item is None: raise RuntimeError('could not locate Stop menu row')
        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab', '-i', 'desktop',
                        '-frames:v', '1', r'F:\dlss5-video\dlss5-video-amd\cast-stop-menu.png'], timeout=30)
        click(*stop_item)
        print('clicked Stop row; observing TV state for 8s', flush=True)

        for i in range(4):
            time.sleep(2)
            after = tv_state(control)
            print('  t+%ds state=%s' % ((i + 1) * 2, after), flush=True)
            if after == 'STOPPED': break
        hls_after = http_status('/live.m3u8')
        print('STOPPED_STATE %s' % after, flush=True)
        print('HLS_PORT status=%d (0/4xx/5xx = closed)' % hls_after, flush=True)
        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab', '-i', 'desktop',
                        '-frames:v', '1', a.shot], timeout=30)
        print('screenshot:', os.path.getsize(a.shot) if os.path.exists(a.shot) else 'NONE', flush=True)
    finally:
        if hwnd is not None: u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(1)
        if proc.poll() is None: proc.kill()

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []
    print('--- stop evidence lines ---', flush=True)
    for line in lines:
        if 'Cast' in line or 'cast' in line.lower(): print('  ', line, flush=True)

    checks = {
        'cast was streaming': hls_during == 200 and before == 'PLAYING',
        'stop row located': stop_item is not None,
        'TV reports STOPPED': after == 'STOPPED',
        'HLS port closed': hls_after != 200,
        'no start failure': not any(x in l for l in lines for x in
                                    ('Cast start failed', 'SetAVTransportURI failed', 'Play failed')),
        'toast stopped': any('cast.stopped' in l or 'stopped' in l.lower() for l in lines),
    }
    print('--- verdict ---', flush=True)
    for k, v in checks.items():
        print('%-22s %s' % (k, 'PASS' if v else 'FAIL'), flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
