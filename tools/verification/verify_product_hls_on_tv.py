"""Layered DLNA check: product-encoded segments + product HTTP server -> TV.

Isolates the data plane from the renderer capture path. The HLS directory must
already contain product-parameter-encoded segments (see the layered runbook).
This script:
  1. starts cast_http_runner.exe (the shipped CastHttpServer.cpp) on port 8080
  2. verifies local GET /live.m3u8 has the corrected Content-Type
  3. pushes the URL to the TV via the validated dlna_sender SOAP path
  4. watches GetTransportInfo state and TV->8080 TCP connections
  5. tears everything down in one process (sandbox kills orphaned servers)

Must see (judge lines):
  LOCAL_M3U8_STATUS 200   LOCAL_M3U8_TYPE application/vnd.apple.mpegurl
  DMR_STATE PLAYING       TV_CONNECTIONS > 0
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
POC = os.path.normpath(os.path.join(HERE, '..', 'dlna-poc'))
sys.path.insert(0, POC)
import dlna_sender as d  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
RUNNER = os.path.join(REPO, 'build-ninja', 'cast_http_runner.exe')


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def detect_local_ip(tv_ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.connect((tv_ip, 63636))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def tv_connections(tv_ip, port):
    out = subprocess.run(['netstat', '-ano'], capture_output=True, timeout=10).stdout.decode('utf-8', 'replace')
    pat = re.compile(r'^\s*TCP\s+\S+:' + str(port) + r'\s+' + re.escape(tv_ip) + r':\d+\s+(ESTABLISHED|TIME_WAIT)',
                     re.I)
    return sum(1 for line in out.splitlines() if pat.search(line))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True, help='directory holding product-encoded live.m3u8 + seg-*.ts')
    ap.add_argument('--generate', action='store_true',
                    help='continuously encode a synthetic source with the product ffmpeg parameters')
    ap.add_argument('--tv', default='192.168.1.24')
    ap.add_argument('--local-ip', default=None)
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--control', default=None, help='AVTransport control URL (skip SSDP if given)')
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    local_ip = args.local_ip or detect_local_ip(args.tv) or '192.168.1.31'
    log('local=%s tv=%s dir=%s' % (local_ip, args.tv, args.dir))

    encoder = None
    if args.generate:
        # Byte-for-byte the product CastEncoder.cpp parameters, with a synthetic
        # source standing in for the missing renderer capture. -re keeps the live
        # playlist advancing so the TV never sees ENDLIST mid-observation.
        ffmpeg = os.path.join(REPO, 'build-ninja', 'ffmpeg.exe')
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
               '-f', 'lavfi', '-re', '-i', 'testsrc2=size=1920x1080:rate=30',
               '-f', 'lavfi', '-re', '-i', 'sine=frequency=440:sample_rate=48000',
               '-map', '1:a:0', '-map', '0:v:0',
               '-c:v', 'h264_amf', '-quality', 'speed', '-b:v', '8000k',
               '-pix_fmt', 'yuv420p', '-g', '30',
               '-c:a', 'aac', '-b:a', '128k',
               '-f', 'hls', '-hls_time', '2', '-hls_list_size', '12',
               '-hls_flags', 'delete_segments', '-hls_segment_type', 'mpegts',
               '-hls_segment_filename', os.path.join(args.dir, 'seg-%06d.ts'),
               os.path.join(args.dir, 'live.m3u8')]
        encoder = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        log('encoder started pid=%d' % encoder.pid)
        # Wait until at least 3 segments exist before pushing to the DMR.
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                text = open(os.path.join(args.dir, 'live.m3u8'), encoding='utf-8').read()
                if text.count('#EXTINF:') >= 3:
                    break
            except OSError:
                pass
            time.sleep(0.3)
        else:
            log('ERROR segments not ready')
            encoder.kill()
            return 2

    if not os.path.isfile(os.path.join(args.dir, 'live.m3u8')):
        log('ERROR no live.m3u8 in ' + args.dir)
        return 2

    runner = subprocess.Popen([RUNNER, args.dir, str(args.port)],
                              cwd=os.path.dirname(RUNNER),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    control = None
    try:
        time.sleep(1.5)
        url = 'http://%s:%d/live.m3u8' % (local_ip, args.port)
        r = urllib.request.urlopen(url, timeout=8)
        print('LOCAL_M3U8_STATUS', r.status)
        print('LOCAL_M3U8_TYPE', r.headers.get('Content-Type'))
        r.read()

        if args.control:
            control = args.control
        else:
            locs = d.ssdp_discover(local_ip, 8)
            for loc in locs:
                try:
                    friendly, base, ctl = d.parse_description(loc)
                except Exception:
                    continue
                if any(k in friendly for k in ('CAST', '投屏', 'DMR')):
                    control = ctl
                    log('DMR=%s' % friendly)
                    break
        if not control:
            print('JUDGE FAIL reason=no_dmr')
            return 3

        url = 'http://%s:%d/live.m3u8' % (local_ip, args.port)
        d.soap_call(control, d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
        status, raw = d.soap_call(control, d.AVTRANSPORT, 'SetAVTransportURI', {
            'InstanceID': '0', 'CurrentURI': url,
            'CurrentURIMetaData': d.didl_metadata(url, 'application/x-mpegURL', 'Product HLS'),
        })
        log('SetAVTransportURI -> %s' % status)
        d.soap_call(control, d.AVTRANSPORT, 'Play', {'InstanceID': '0'})

        playing = 0
        samples = 0
        max_tv = 0
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            time.sleep(1)
            samples += 1
            try:
                st, info = d.soap_call(control, d.AVTRANSPORT, 'GetTransportInfo', {'InstanceID': '0'})
                state = d.parse_tag(info, 'CurrentTransportState')
                if state == 'PLAYING':
                    playing += 1
            except Exception:
                state = '?'
            max_tv = max(max_tv, tv_connections(args.tv, args.port))
            if samples % 5 == 0:
                log('t=%ds state=%s tv_conns=%d' % (samples, state, max_tv))
        print('DMR_STATE', 'PLAYING' if playing >= samples * 0.6 else 'NOT_PLAYING',
              'playing=%d/%d' % (playing, samples))
        print('TV_CONNECTIONS', max_tv)
        ok = playing >= samples * 0.6 and max_tv > 0
        print('JUDGE', 'PASS' if ok else 'FAIL')
        return 0 if ok else 1
    finally:
        if control:
            try:
                d.soap_call(control, d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
            except Exception:
                pass
        runner.terminate()
        try:
            runner.wait(timeout=5)
        except subprocess.TimeoutExpired:
            runner.kill()
        if encoder is not None and encoder.poll() is None:
            encoder.terminate()
            try:
                encoder.wait(timeout=5)
            except subprocess.TimeoutExpired:
                encoder.kill()


if __name__ == '__main__':
    raise SystemExit(main())
