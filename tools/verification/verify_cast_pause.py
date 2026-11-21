#!/usr/bin/env python3
"""Verify the pause/resume path keeps the HLS live and the TV in sync.

Design claim (CastSession.h 14.4/14.6 / SetPaused comment): while the player is
paused the render loop stops, but CastSession re-feeds the cached last frame so
HLS never breaks.  This script tests the OBSERVABLE consequence on a real TV:

  1. cast, confirm TV DMR = PLAYING and HLS slices are advancing
  2. pause via WM_COMMAND/IDM_PLAY (TogglePause -> SetPaused(true))
  3. sample the HLS playlist and TV state every 2s during the pause
  4. resume via the same command, sample again

Judge lines:
  BASELINE_SLICES advancing        before pause
  PAUSE_SLICES   a,b,c,...         sequence during pause
  PAUSE_HLS_LIVE yes/no            did seg numbers keep advancing while paused
  PAUSE_TV_STATE PLAYING/...       TV state during pause
  RESUME_SLICES advancing          after resume

Does not trust netstat (404 opens sockets).  The slice number from live.m3u8 is
the proof that frames reached the encoder.  Run with workstation python on the
interactive desktop.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
POC = os.path.normpath(os.path.join(HERE, '..', 'dlna-poc'))
sys.path.insert(0, POC)
import dlna_sender as d  # noqa: E402

u = ctypes.windll.user32
WM_CLOSE, WM_COMMAND = 0x0010, 0x0111
IDM_PLAY = 200
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


def http_get(path, timeout=5):
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
        head, _, body = buf.partition(b'\r\n\r\n')
        m = re.search(rb'HTTP/1\.1\s+(\d{3})', head)
        return (int(m.group(1)) if m else 0), body
    except Exception:
        return 0, b''
    finally:
        if s: s.close()


def last_slice():
    """Highest seg-NNNNNN.ts number currently listed in live.m3u8, or -1."""
    status, body = http_get('/live.m3u8')
    if status != 200: return -1
    nums = [int(n) for n in re.findall(rb'seg-(\d+)\.ts', body)]
    return max(nums) if nums else -1


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


def sample(seconds, every, control, label):
    seq, states = [], []
    for _ in range(max(1, seconds // every)):
        time.sleep(every)
        seq.append(last_slice())
        states.append(tv_state(control) if control else '?')
    print('%s_SLICES %s' % (label, ','.join(str(x) for x in seq)), flush=True)
    print('%s_TV %s' % (label, ','.join(states)), flush=True)
    return seq, states


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--shot', default=r'F:\dlss5-video\dlss5-video-amd\cast-pause.png')
    a = ap.parse_args()

    if os.path.exists(a.log): os.remove(a.log)
    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    control = None
    base, pause_seq, resume_seq = [], [], []
    pause_states = []
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

        click(a.cast_x, a.cast_y); time.sleep(9)
        click(a.item_x, a.item_y)
        time.sleep(6)
        friendly, control = find_dmr()
        print('DMR=%s control=%s' % (friendly, control), flush=True)
        print('pre-pause TV=%s hls=%d' % (tv_state(control) if control else '?', last_slice()), flush=True)

        base, _ = sample(6, 2, control, 'BASELINE')

        # Pause: WM_COMMAND/IDM_PLAY routes to TogglePause -> SetPaused(true).
        u.PostMessageW(hwnd, WM_COMMAND, IDM_PLAY, 0)
        print('sent WM_COMMAND IDM_PLAY (pause)', flush=True)
        pause_seq, pause_states = sample(12, 2, control, 'PAUSE')

        # Resume.
        u.PostMessageW(hwnd, WM_COMMAND, IDM_PLAY, 0)
        print('sent WM_COMMAND IDM_PLAY (resume)', flush=True)
        resume_seq, _ = sample(8, 2, control, 'RESUME')

        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab', '-i', 'desktop',
                        '-frames:v', '1', a.shot], timeout=30)
        print('screenshot:', os.path.getsize(a.shot) if os.path.exists(a.shot) else 'NONE', flush=True)
    finally:
        if hwnd is not None: u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(1)
        if proc.poll() is None: proc.kill()

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []

    def advancing(seq):
        valid = [x for x in seq if x >= 0]
        return len(valid) >= 2 and valid[-1] > valid[0]

    pause_live = advancing(pause_seq)
    checks = {
        'baseline advancing': advancing(base),
        'pause HLS stays live': pause_live,
        'pause TV still PLAYING': all(s == 'PLAYING' for s in pause_states) and bool(pause_states),
        'resume advancing': advancing(resume_seq),
        'no cast failure': not any(x in l for l in lines for x in
                                   ('Cast start failed', 'Cast encoder stopped unexpectedly')),
    }
    print('--- verdict ---', flush=True)
    for k, v in checks.items():
        print('%-24s %s' % (k, 'PASS' if v else 'FAIL'), flush=True)
    print('PAUSE_HLS_LIVE', 'yes' if pause_live else 'no', flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
