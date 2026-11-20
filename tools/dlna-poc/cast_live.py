#!/usr/bin/env python3
"""一键 DLNA 局域网 HLS 投屏（已验证：国行索尼 X9500H / SMG_CAST_PLAYER）。

把肉眼验证成功的链路固化为单进程工具：
  ffmpeg 实时生成 HLS -> CORS/Range HTTP 服务 -> SSDP/指定 DMR
  -> Stop -> SetAVTransportURI(live.m3u8) -> Play -> 播放头 + 拉流证据监控。

三条硬教训（2026-11-20）：
  1. 服务必须和推流在【同一个前台进程】里管理生命周期。否则工具调用结束时
     后台进程被沙箱回收，电视来拉流时 8080 已无人监听——这是头号假失败源，
     曾让我们误判为防火墙/明文/私网 IP/容器问题，绕了一大圈。
  2. 这台电视认 HLS(m3u8+ts)，不认裸 .ts（声明成 video/mp4 或 video/mpeg 都不拉）。
  3. 判成功必须同时看「GetPositionInfo 前进」+「access.log 有电视 GET」；
     只看 SOAP 200 会被 DMR 的“礼貌性接受”骗到。

用法：
  python3 cast_live.py --seconds 90                       # 彩色测试图（肉眼验证画面在动）
  python3 cast_live.py --tv 192.168.1.24 --seconds 60
  python3 cast_live.py --source /path/to/video.mp4        # 用真实片源循环推流
  python3 cast_live.py --check-only                       # 只自检服务+发现，不投流
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

import dlna_sender as d

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FFMPEG = os.path.join(REPO, 'external', 'ffmpeg', 'bin', 'ffmpeg.exe')
SERVE_HLS = os.path.join(REPO, 'tools', 'cast-poc', 'serve_hls.py')
ACCESS_LOG = os.path.join(REPO, 'tools', 'cast-poc', 'access.log')


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def detect_local_ip(tv_ip):
    """拿本机与电视同网段的出口 IP（连电视地址不会真的发数据）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.connect((tv_ip, 63636))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def resolve_dmr(args, local_ip):
    """返回 AVTransport control URL。--desc 优先，否则 SSDP 发现。"""
    if args.desc:
        friendly, base, control = d.parse_description(args.desc)
        return control, friendly
    if args.control:
        return args.control, 'DMR'

    log('SSDP 发现 DMR（local ip %s）...' % local_ip)
    locs = d.ssdp_discover(local_ip, args.discover_seconds)
    if not locs:
        return None, None
    if args.tv:
        hit = next((l for l in locs if args.tv in l), None)
        if hit:
            locs = [hit] + [l for l in locs if l != hit]
    for loc in locs:
        try:
            friendly, base, control = d.parse_description(loc)
        except Exception:
            continue
        # 优先选投屏渲染器（friendlyName 含 CAST/投屏/DMR），跳过路由器等
        if any(k in friendly for k in ('CAST', '投屏', 'DMR', 'TV', '电视')):
            return control, friendly
    friendly, base, control = d.parse_description(locs[0])
    return control, friendly


def wait_segments(hls_dir, timeout=20):
    """等 m3u8 出现且 >=3 个切片，避免电视拉到空播放列表。"""
    m3u8 = os.path.join(hls_dir, 'live.m3u8')
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            text = open(m3u8, encoding='utf-8').read()
            if text.count('#EXTINF:') >= 3:
                return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def access_log_lines():
    try:
        with open(ACCESS_LOG, encoding='utf-8', errors='replace') as fh:
            return sum(1 for _ in fh)
    except OSError:
        return 0


def count_tv_hits(since_line, tv_ip):
    try:
        with open(ACCESS_LOG, encoding='utf-8', errors='replace') as fh:
            lines = fh.readlines()[since_line:]
        return sum(1 for ln in lines if (tv_ip or '192.168.1.24') in ln)
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tv', help='电视 IP（用于选出口 IP / 匹配 SSDP 结果）')
    ap.add_argument('--local-ip', help='本机局域网 IP（默认自动检测）')
    ap.add_argument('--desc', help='直接给 description.xml URL，跳过 SSDP')
    ap.add_argument('--control', help='直接给 AVTransport control URL')
    ap.add_argument('--discover-seconds', type=int, default=10)
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--seconds', type=int, default=90, help='推流/监控时长')
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--source', help='视频文件；不给则用 testsrc2 彩色测试图+440Hz')
    ap.add_argument('--mime', default='application/x-mpegURL')
    ap.add_argument('--title', default='DLNA Live')
    ap.add_argument('--out', default='cast-live-trace.jsonl')
    ap.add_argument('--check-only', action='store_true', help='只自检服务+发现，不投流')
    args = ap.parse_args()

    if not os.path.isfile(FFMPEG):
        log('ERROR ffmpeg 不存在: %s' % FFMPEG)
        return 2

    tv_ip = args.tv or '192.168.1.24'
    local_ip = args.local_ip or detect_local_ip(tv_ip) or '192.168.1.31'
    log('本机 IP=%s  电视 IP=%s' % (local_ip, tv_ip))

    hls_dir = tempfile.mkdtemp(prefix='dlna-live-')
    ffmpeg_proc = None
    serve_proc = None
    try:
        # 1) 实时生成 HLS
        cmd = [FFMPEG, '-hide_banner', '-nostdin', '-loglevel', 'error', '-y']
        if args.source:
            cmd += ['-re', '-stream_loop', '-1', '-i', args.source,
                    '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
                    '-g', '60', '-c:a', 'aac', '-b:a', '128k']
        else:
            cmd += ['-f', 'lavfi', '-re', '-i', 'testsrc2=size=1280x720:rate=30',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                    '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
                    '-g', '60', '-c:a', 'aac', '-b:a', '128k']
        cmd += ['-f', 'hls', '-hls_time', '2', '-hls_list_size', '12',
                '-hls_flags', 'delete_segments', '-hls_segment_type', 'mpegts',
                '-hls_segment_filename', os.path.join(hls_dir, 'seg-%06d.ts'),
                os.path.join(hls_dir, 'live.m3u8')]
        ffmpeg_proc = subprocess.Popen(cmd)
        log('ffmpeg 启动 (pid=%d)' % ffmpeg_proc.pid)

        # 2) CORS/Range HTTP 服务
        serve_proc = subprocess.Popen([sys.executable, SERVE_HLS, '--port', str(args.port),
                                       '--bind', '0.0.0.0', '--directory', hls_dir])
        log('HTTP 服务启动 (pid=%d) port=%d' % (serve_proc.pid, args.port))

        # 3) 等切片就绪 + 本地自检
        if not wait_segments(hls_dir):
            log('ERROR HLS 切片未就绪')
            return 3
        stream_url = 'http://%s:%d/live.m3u8' % (local_ip, args.port)
        try:
            code = urllib.request.urlopen(stream_url, timeout=5).status
        except Exception as exc:
            log('ERROR 本地访问 %s 失败: %s' % (stream_url, exc))
            return 3
        log('本地自检 %s -> HTTP %s  ✓' % (stream_url, code))

        # 4) 发现 DMR
        control, friendly = resolve_dmr(args, local_ip)
        if not control:
            log('ERROR 未发现 DMR')
            return 4
        log('DMR=%s control=%s' % (friendly, control))

        if args.check_only:
            log('--check-only：基础设施自检通过，未投流。')
            return 0

        # 5) Stop -> SetURI -> Play（小米栈要求先 Stop）
        d.soap_call(control, d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
        status, raw = d.soap_call(control, d.AVTRANSPORT, 'SetAVTransportURI', {
            'InstanceID': '0', 'CurrentURI': stream_url,
            'CurrentURIMetaData': d.didl_metadata(stream_url, args.mime, args.title),
        })
        log('SetAVTransportURI -> HTTP %s' % status)
        if status >= 400:
            log(raw[:800])
            return 5
        status, raw = d.soap_call(control, d.AVTRANSPORT, 'Play',
                                  {'InstanceID': '0', 'Speed': '1'})
        log('Play -> HTTP %s' % status)
        if status >= 400:
            log(raw[:800])
            return 6

        # 6) 播放头 + 拉流证据监控
        since = access_log_lines()
        start = time.time()
        end = start + args.seconds
        rows = []
        first_ms = None
        with open(args.out, 'w', encoding='utf-8') as fh:
            while time.time() < end:
                now = time.time()
                t_ms = int((now - start) * 1000)
                _, pos_raw = d.soap_call(control, d.AVTRANSPORT, 'GetPositionInfo',
                                         {'InstanceID': '0'})
                _, tr_raw = d.soap_call(control, d.AVTRANSPORT, 'GetTransportInfo',
                                        {'InstanceID': '0'})
                rel = d.parse_tag(pos_raw, 'RelTime')
                abs_t = d.parse_tag(pos_raw, 'AbsTime')
                state = d.parse_tag(tr_raw, 'CurrentTransportState')
                pos_s = d.parse_time(rel if rel and rel != 'NOT_IMPLEMENTED' else abs_t)
                if pos_s is not None and first_ms is None and pos_s > 0:
                    first_ms = t_ms
                hits = count_tv_hits(since, tv_ip)
                row = {'t_ms': t_ms, 'state': state, 'rel_time': rel,
                       'pos_s': pos_s, 'tv_http_hits': hits}
                rows.append(row)
                fh.write(__import__('json').dumps(row, ensure_ascii=False) + '\n')
                fh.flush()
                log('t=%5dms state=%-10s pos=%-10s TV请求=%d' % (t_ms, state, rel, hits))
                time.sleep(max(0, args.interval - (time.time() - now)))

        hits = count_tv_hits(since, tv_ip)
        states = [r['state'] for r in rows]
        poses = [r['pos_s'] for r in rows if r.get('pos_s') is not None]
        advancing = bool(poses and poses[-1] > poses[0])
        log('WROTE %s rows=%d first_frame_ms=%s' % (args.out, len(rows), first_ms))
        log('判决: PLAYING=%d/%d  播放头前进=%s(%.1fs→%.1fs)  电视HTTP请求=%d' % (
            states.count('PLAYING'), len(states), advancing,
            poses[0] if poses else -1, poses[-1] if poses else -1, hits))
        log('判定: %s' % ('✓ 成功（电视确实在拉流）' if (hits > 0 and advancing)
                          else '✗ 失败（查 access.log / 服务是否存活）'))
        d.soap_call(control, d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
        return 0 if (hits > 0 and advancing) else 1
    finally:
        for proc in (ffmpeg_proc, serve_proc):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        shutil.rmtree(hls_dir, ignore_errors=True)
        log('已清理 ffmpeg / HTTP 服务 / 临时切片目录。')


if __name__ == '__main__':
    sys.exit(main() or 0)
