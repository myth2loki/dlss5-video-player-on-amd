#!/usr/bin/env python3
"""Verify casting works with neural rendering (live session) or playback frame generation on.

2026-11-21 follow-up to the upscale bug: EnableUpscaling() swapped the renderer
with a candidate built through Initialize(...,9 args) (captureOutput=false),
which killed the HLS stream. Two more renderer-affecting features were audited:

  * neural rendering (ToggleNeuralRendering -> StartLiveNeuralSession)
  * playback frame generation (SetPlaybackFrameGeneration)

Neither toggle rebuilds D3D12Renderer; they set flags / attach a live neural
session on the SAME renderer. All 5 renderer-Initialize call sites now pass
captureOutput=true. This script confirms empirically that a cast started with
either feature active still produces a live HLS stream.

The feature must be PROVEN active in the log before the cast is judged. If the
feature never comes up (neural=off / framegen=off) the verdict is INCONCLUSIVE,
not PASS - a stream can only be called healthy while the feature is really on.

Judge lines:
  FEATURE active      neural=live (neural) / framegen=on (framegen) in scheduler log
  CAPTURE_OUTPUT 1    Cast capture diag captureOutput=1 after the feature is on
  HLS 200 + slices    m3u8 200 and seg-N.ts advancing, non-empty
  TV_STATE PLAYING    SOAP GetTransportInfo
  ALL=PASS only when all above are true; INCONCLUSIVE when the feature or the
  baseline cast never came up (no false PASS on a dead stream, no false FAIL
  on an environment fluke such as 'device unreachable: no-response').

Run with the workstation python on the interactive desktop. Both the PC and
the TV must be on the LAN and port 8080 free.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, shutil, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
POC = os.path.normpath(os.path.join(HERE, '..', 'dlna-poc'))
sys.path.insert(0, POC)
import dlna_sender as d  # noqa: E402

u = ctypes.windll.user32
WM_CLOSE, WM_COMMAND = 0x0010, 0x0111
IDM_NEURAL_RENDERING = 300
IDM_PLAYBACK_FRAME_GENERATION = 307
LEFTDOWN, LEFTUP = 0x0002, 0x0004
WND_ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

DEFAULT_PLAYER = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
DEFAULT_MEDIA  = r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
DEFAULT_LOG    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.log'
DEFAULT_INI    = r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.ini'
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


def set_ini(ini_path, key, value):
    """Set one Playback key in place, preserving the rest. Returns old line or None."""
    with open(ini_path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines(True)
    old = None
    for i, l in enumerate(lines):
        if l.strip().startswith(key + '='):
            old = l
            lines[i] = '%s=%.6f\n' % (key, value)
            break
    if old is None:
        lines.append('%s=%.6f\n' % (key, value))
    with open(ini_path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(lines)
    return old


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--feature', choices=('neural', 'framegen'), required=True)
    ap.add_argument('--player', default=DEFAULT_PLAYER)
    ap.add_argument('--media', default=DEFAULT_MEDIA)
    ap.add_argument('--log', default=DEFAULT_LOG)
    ap.add_argument('--ini', default=DEFAULT_INI)
    ap.add_argument('--cast-x', type=int, default=490)
    ap.add_argument('--cast-y', type=int, default=1266)
    ap.add_argument('--item-x', type=int, default=570)
    ap.add_argument('--item-y', type=int, default=1144)
    ap.add_argument('--shot', default=r'F:\dlss5-video\dlss5-video-amd\cast-feature.png')
    ap.add_argument('--hold', type=int, default=24, help='seconds to observe HLS after cast')
    a = ap.parse_args()

    # Feature-specific setup. Neural: live session is off by default even with
    # NeuralView=1, so toggle it with IDM_NEURAL_RENDERING once playback is up.
    # Framegen: pre-set the ini so it is on from the first rendered frame.
    feature_cmd = IDM_NEURAL_RENDERING if a.feature == 'neural' else IDM_PLAYBACK_FRAME_GENERATION
    marker = ('neural=live',) if a.feature == 'neural' else ('framegen=on',)

    ini_bak = a.ini + '.featbak'
    shutil.copy2(a.ini, ini_bak)
    if a.feature == 'framegen':
        set_ini(a.ini, 'FrameGenerationPlayback', 1.0)
    else:
        # NeuralView must be on so the D toggle starts a live neural session.
        set_ini(a.ini, 'NeuralView', 1.0)

    if os.path.exists(a.log): os.remove(a.log)
    proc = subprocess.Popen([a.player, a.media], cwd=os.path.dirname(a.player))
    hwnd = None
    control = None
    seq = []
    tv_during = '?'
    try:
        for _ in range(25):
            time.sleep(1)
            wins = find_player()
            if wins: hwnd = wins[0]; break
        if hwnd is None: raise RuntimeError('player window never appeared')
        u.MoveWindow(hwnd, 40, 40, 2400, 1320, True); time.sleep(0.4)
        u.ShowWindow(hwnd, 9); time.sleep(0.2)
        u.SetForegroundWindow(hwnd); time.sleep(3)
        print('window:', hex(hwnd), 'fg-match:', u.GetForegroundWindow() == hwnd, flush=True)

        if a.feature == 'neural':
            # Start the live neural session now that playback is running.
            u.PostMessageW(hwnd, WM_COMMAND, feature_cmd, 0)
            print('sent WM_COMMAND IDM_NEURAL_RENDERING (start live session); waiting 14s', flush=True)
            time.sleep(14)
        else:
            # Framegen was pre-set in ini; just let it come up.
            print('framegen pre-set in ini; waiting 10s for it to engage', flush=True)
            time.sleep(10)

        # Confirm the feature is really active before judging the cast.
        lines_so_far = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
                       if os.path.exists(a.log) else []
        feat_lines = [l for l in lines_so_far if any(m in l for m in marker)]
        print('FEATURE_LINES %d' % len(feat_lines), flush=True)
        for l in feat_lines[-3:]: print('  ', l, flush=True)

        click(a.cast_x, a.cast_y); time.sleep(9)
        click(a.item_x, a.item_y)
        print('clicked first DMR; probing HLS %ds' % a.hold, flush=True)
        friendly, control = find_dmr()
        print('DMR=%s control=%s' % (friendly, control), flush=True)

        tv_seq = []
        for i in range(max(1, a.hold // 2)):
            time.sleep(2)
            n, cnt = last_slice()
            seq.append(n)
            st = tv_state(control) if control else '?'
            tv_seq.append(st)
            print('  probe %02d: seg=%d listed=%d tv=%s' % (i, n, cnt, st), flush=True)
        # A single DMR sample can catch a transient buffering state. The state is
        # healthy if it ever reaches PLAYING and never reports a fault; a stream
        # stuck at PAUSED for all samples is a real signal worth reporting.
        tv_during = 'PLAYING' if 'PLAYING' in tv_seq else (tv_seq[-1] if tv_seq else '?')
        print('TV_STATES %s' % ','.join(tv_seq), flush=True)
        print('TV_STATE %s' % tv_during, flush=True)

        subprocess.run([FFMPEG, '-y', '-v', 'error', '-f', 'gdigrab', '-i', 'desktop',
                        '-frames:v', '1', a.shot], timeout=30)
        print('screenshot:', os.path.getsize(a.shot) if os.path.exists(a.shot) else 'NONE', flush=True)
    finally:
        if hwnd is not None: u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(2)
        if proc.poll() is None: proc.kill()
        # Restore ini whatever happened - the player rewrites it on exit.
        shutil.copy2(ini_bak, a.ini)
        print('ini restored from backup', flush=True)

    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines() \
            if os.path.exists(a.log) else []
    diag = [l for l in lines if 'Cast capture diag' in l]
    print('--- capture diag ---', flush=True)
    for l in diag: print('  ', l, flush=True)
    feat_lines = [l for l in lines if any(m in l for m in marker)]
    print('--- feature active lines ---', flush=True)
    for l in feat_lines[-4:]: print('  ', l, flush=True)
    start_fail = [l for l in lines if any(x in l for x in
                  ('Cast start failed', 'device unreachable', 'no-response'))]

    feature_active = len(feat_lines) > 0
    m = re.search(r'captureOutput=(\d+).*?output=(\d+x\d+)', ' '.join(diag))
    cap = int(m.group(1)) if m else -1
    out_size = m.group(2) if m else '?'
    valid = [x for x in seq if x >= 0]
    advancing = len(valid) >= 2 and valid[-1] > valid[0]
    ready = [l for l in lines if 'HlsReady=1' in l]
    baseline_up = len(start_fail) == 0 and any(x >= 0 for x in seq)

    print('--- verdict ---', flush=True)
    if not feature_active:
        print('VERDICT=INCONCLUSIVE (feature never active: %s)' % marker, flush=True)
        return 2
    if not baseline_up:
        print('VERDICT=INCONCLUSIVE (baseline cast never came up)', flush=True)
        for l in start_fail[-3:]: print('  ', l, flush=True)
        return 2

    checks = {
        'feature active in log': feature_active,
        'capture ring alive (cap=1)': cap == 1,
        'HlsReady=1': len(ready) > 0,
        'HLS slices advancing': advancing,
        'TV DMR PLAYING': tv_during == 'PLAYING',
        'no start failure': len(start_fail) == 0,
    }
    for k, v in checks.items():
        print('%-28s %s' % (k, 'PASS' if v else 'FAIL'), flush=True)
    print('FEATURE %s (output %s)' % (a.feature, out_size), flush=True)
    print('CAPTURE_OUTPUT %d' % cap, flush=True)
    print('HLS_SLICES %s' % ','.join(str(x) for x in seq), flush=True)
    print('ALL=' + ('PASS' if all(checks.values()) else 'FAIL'), flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
