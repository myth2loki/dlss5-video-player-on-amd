#!/usr/bin/env python3
"""Verify casting works when upscaling is enabled BEFORE the cast starts.

Regression for the 2026-11-21 bug: EnableUpscaling() swaps the active renderer
with a candidate built through Initialize(...,9 args), leaving captureOutput at
its default false. Casting after that produced no HLS slices and the TV showed
nothing, even though the encoder reported started.

Flow: launch -> WM_COMMAND/IDM_DLSS_UPSCALING (enable SR) -> confirm upscale rung
-> cast to first DMR -> judge with the signals that cannot pass on a dead stream:

  CAST CAPTURE DIAG captureOutput=1  (in the log, from D3D12Renderer.cpp)
  HLS m3u8 200 + non-empty seg       (probe, like verify_cast_capture.py)
  TV DMR PLAYING                     (SOAP GetTransportInfo)

Judge lines:
  UPSCALE_ACTIVE yes (output WxH)   SR rung is really on before casting
  CAPTURE_OUTPUT 1                  readback ring alive after the swap
  HLS_SLICES a,b,c                  slices advancing
  TV_STATE PLAYING
Run with the workstation python on the interactive desktop.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
POC = os.path.normpath(os.path.join(HERE, '..', 'dlna-poc'))
sys.path.insert(0, POC)
import dlna_sender as d  # noqa: E402

u = ctypes.windll.user32
WM_CLOSE, WM_COMMAND = 0x0010, 0x0111
IDM_DLSS_UPSCALING = 308
LEFTDOWN, LEFTUP = 0x0002, 0x0004
WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

DEFAULT_PLAYER = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
DEFAULT_MEDIA  = r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
DEFAULT_LOG    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.log'
LOCAL_IP, PORT = '192.168.1.31', 8080
FFMPEG = r'F:\dlss5-video\dlss5-video-amd\build-ninja\ffmpeg.exe'


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


def last_slice():
    s = None
    try:
        s = socket.create_connection((LOCAL_IP, PORT), timeout=4)
        s.sendall(('GET /live.m3u8 HTTP/1.1\r\nHost: ' + LOCAL_IP + ':' + str(PORT) +
                   '\r\nConnection: close\r\n\r\n').encode())
        buf = b''
        while True:
            try: c = s.recv(65536)
            except socket.timeout: break
            if not c: break
            buf += c
            if len(buf) > 8 * 1024 * 1024: break
        head, _, body = buf.partition(b'\r\n\r\n')
        m = re.search(rb'HTTP/1\.1\s+(\d{3})', head)
        if not m or int(m.group(1)) != 200: return -1, 0
        nums = [int(n) for n in re.findall(rb'seg-(\d+)\.ts', body)]
        return (max(nums) if nums else -1), len(nums)
    except Exception:
        return -1, 0
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--shot', default=r'F:\dlss5-video\dlss5-video-amd\cast-upscale.png')
    a = ap.parse_args()

    if os.path.exists(a.log): os.remove(a.log)
    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    control = None
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

        # Enable upscaling first - this is the renderer-swap path that used to
        # leave captureOutput=false.
        u.PostMessageW(hwnd, WM_COMMAND, IDM_DLSS_UPSCALING, 0)
        print('sent WM_COMMAND IDM_DLSS_UPSCALING (enable SR); waiting 8s for rebuild', flush=True)
        time.sleep(8)

        click(a.cast_x, a.cast_y); time.sleep(9)
        click(a.item_x, a.item_y)
        print('clicked first DMR; probing HLS 20s', flush=True)
        friendly, control = find_dmr()
        print('DMR=%s control=%s' % (friendly, control), flush=True)

        seq = []
        for i in range(10):
            time.sleep(2)
            n, cnt = last_slice()
            seq.append(n)
            print('  probe %02d: seg=%d listed=%d' % (i, n, cnt), flush=True)
        print('HLS_SLICES %s' % ','.join(str(x) for x in seq), flush=True)
        print('TV_STATE %s' % (tv_state(control) if control else '?'), flush=True)

        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab', '-i', 'desktop',
                        '-frames:v', '1', a.shot], timeout=30)
        print('screenshot:', os.path.getsize(a.shot) if os.path.exists(a.shot) else 'NONE', flush=True)
    finally:
        if hwnd is not None: u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(1)
        if proc.poll() is None: proc.kill()

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []
    diag = [l for l in lines if 'Cast capture diag' in l]
    print('--- capture diag ---', flush=True)
    for l in diag: print('  ', l, flush=True)
    ready = [l for l in lines if 'playlist hold done' in l]
    print('--- playlist readiness ---', flush=True)
    for l in ready[-4:]: print('  ', l, flush=True)

    m = re.search(r'captureOutput=(\d+).*?output=(\d+x\d+).*?upscaleActive=(\d+)', ' '.join(diag))
    cap = int(m.group(1)) if m else -1
    out_size = m.group(2) if m else '?'
    up_active = int(m.group(3)) if m else -1
    valid = [x for x in seq if x >= 0]
    advancing = len(valid) >= 2 and valid[-1] > valid[0]
    checks = {
        'upscale enabled before cast': up_active == 1,
        'capture ring alive (cap=1)': cap == 1,
        'HlsReady=1': any('HlsReady=1' in l for l in ready),
        'HLS slices advancing': advancing,
        'no start failure': not any(x in l for l in lines for x in
                                    ('Cast start failed', 'SetAVTransportURI failed', 'Play failed')),
    }
    print('--- verdict ---', flush=True)
    for k, v in checks.items():
        print('%-28s %s' % (k, 'PASS' if v else 'FAIL'), flush=True)
    print('UPSCALE_ACTIVE %d (output %s)' % (up_active, out_size), flush=True)
    print('CAPTURE_OUTPUT %d' % cap, flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
