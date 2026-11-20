#!/usr/bin/env python3
"""DLNA 投屏控制台后端 — 设备身份模型(UDN) + 重新扫描 + 切换设备。

单进程托管：
  - 静态页面 GET /  -> cast_console.html
  - REST API        -> 设备发现/探活/切换/停止/状态
  - HLS 会话        -> ffmpeg 子进程 + serve_hls.py 子进程（统一收尾）

设备身份用 UDN（uuid:...），不用 IP / friendlyName：
  - IP 会因 DHCP 变、friendlyName 会重名；UDN 是 UPnP 标准唯一标识。
  - 缓存 key = UDN；同名显示加 UDN 尾 5 位消歧。
  - 设备注册表带 lastSeen，TTL 90s 未再发现则标离线（仍保留，供切换历史）。
  - 点击投屏前强制 probe_device(GetTransportInfo)，杜绝"能选但什么也不做"。

API:
  GET  /                      控制台页面
  GET  /api/devices?refresh=1 扫描并返回设备列表（refresh=1 强制重扫）
  POST /api/cast   {"udn": "..."}   切换投到指定设备
  POST /api/stop   {}                停止投屏
  GET  /api/status                  当前会话状态（目标/播放头/拉流证据）
  GET  /api/source                  当前片源
  POST /api/source {"path": "..."}  切换片源并重启 HLS

用法:
  python3 cast_controller.py                # 测试图 + 440Hz（肉眼验证画面在动）
  python3 cast_controller.py --source VIDEO # 真实片源循环
  python3 cast_controller.py --port 8090 --hls-port 8080
"""
import argparse
import atexit
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import dlna_sender as d

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
FFMPEG = os.path.join(REPO, 'external', 'ffmpeg', 'bin', 'ffmpeg.exe')
SERVE_HLS = os.path.join(REPO, 'tools', 'cast-poc', 'serve_hls.py')
ACCESS_LOG = os.path.join(REPO, 'tools', 'cast-poc', 'access.log')
CONSOLE_HTML = os.path.join(HERE, 'cast_console.html')
LAST_DEVICE_FILE = os.path.join(HERE, 'last_device.json')

TTL_SECONDS = 90


def now():
    return time.time()


def load_last_udn():
    try:
        with open(LAST_DEVICE_FILE, encoding='utf-8') as fh:
            return json.load(fh).get('udn', '')
    except Exception:
        return ''


def save_last_udn(udn):
    try:
        with open(LAST_DEVICE_FILE, 'w', encoding='utf-8') as fh:
            json.dump({'udn': udn}, fh, ensure_ascii=False)
    except Exception:
        pass


class DeviceRegistry:
    """设备注册表：key=UDN，TTL 过期标离线，同名加 UDN 尾号消歧。"""

    def __init__(self, local_ip):
        self.local_ip = local_ip
        self.devices = {}          # udn -> dev dict
        self.lock = threading.RLock()
        self.last_udn = load_last_udn()
        self.last_scan = 0

    def scan(self, seconds=8, probe=True):
        found = d.discover_devices(self.local_ip, seconds)
        # 可达性探活（不是"能否投"判决）：
        #   - 有 AVTransport 的设备发 GetTransportInfo；200/501 fault 都算可达。
        #     501 "Action Failed" 是无播放实例时的合法响应，不代表离线。
        #   - 无 AVTransport 的（索尼 :7084 的 DIAL/ScalarWebAPI）不是 DMR，
        #     直接标 nonDmr，绝不让它混进可投列表（B站靠它唤醒 app，我们不走这条）。
        # 真正"能否投我们的 URL"只在 cast_to 的 SetURI/Play 阶段判定。
        if probe:
            for dev in found:
                if not dev.get('hasAVTransport', True):
                    dev['probeOk'] = False
                    dev['probeState'] = 'nonDmr'
                else:
                    ok, detail = d.probe_device(dev['controlURL'])
                    dev['probeOk'] = ok
                    dev['probeState'] = detail
        with self.lock:
            stamp = now()
            for dev in found:
                key = dev['udn'] or dev['location']
                dev['lastSeen'] = stamp
                dev['online'] = dev.get('probeOk', True)
                self.devices[key] = dev
            # 未在本轮发现的：若超过 TTL 则标离线（不删除，保留切换历史）
            for key, dev in self.devices.items():
                if stamp - dev.get('lastSeen', 0) > TTL_SECONDS:
                    dev['online'] = False
            self.last_scan = stamp
        return len(found)

    def list_devices(self):
        with self.lock:
            all_devs = list(self.devices.values())
            out = []
            for dev in sorted(all_devs, key=lambda x: (not x.get('online'), x.get('friendlyName', ''))):
                item = dict(dev)
                item['displayName'] = d.display_name(dev, all_devs)
                item['isLast'] = bool(self.last_udn and dev['udn'] == self.last_udn)
                out.append(item)
            return out

    def get(self, udn):
        with self.lock:
            return dict(self.devices[udn]) if udn in self.devices else None

    def mark_last(self, udn):
        self.last_udn = udn
        save_last_udn(udn)


class CastSession:
    """HLS 流 + 投屏目标会话。切设备只改 SOAP 目标，HLS 流不变。"""

    def __init__(self, args):
        self.args = args
        self.local_ip = args.local_ip
        self.hls_port = args.hls_port
        self.source = args.source or ''
        self.hls_dir = None
        self.ffmpeg_proc = None
        self.serve_proc = None
        self.target = None          # dev dict of current cast target
        self.monitor_thread = None
        self.stop_monitor = threading.Event()
        self.lock = threading.RLock()
        self.status_data = {
            'streaming': False, 'target': None, 'state': '',
            'pos_s': None, 'tv_http_hits': 0, 'first_frame_ms': None,
            'message': '未投屏',
        }
        self.stream_url = 'http://%s:%d/live.m3u8' % (self.local_ip, self.hls_port)

    # ---- HLS 数据面 ----
    def start_stream(self):
        with self.lock:
            if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None and self.serve_proc and self.serve_proc.poll() is None:
                return True
            self.hls_dir = tempfile.mkdtemp(prefix='dlna-console-')
            cmd = [FFMPEG, '-hide_banner', '-nostdin', '-loglevel', 'error', '-y']
            if self.source and os.path.isfile(self.source):
                cmd += ['-re', '-stream_loop', '-1', '-i', self.source,
                        '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
                        '-g', '60', '-c:a', 'aac', '-b:a', '128k']
            else:
                cmd += ['-f', 'lavfi', '-re', '-i', 'testsrc2=size=1280x720:rate=30',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                        '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
                        '-g', '60', '-c:a', 'aac', '-b:a', '128k']
            cmd += ['-f', 'hls', '-hls_time', '2', '-hls_list_size', '12',
                    '-hls_flags', 'delete_segments', '-hls_segment_type', 'mpegts',
                    '-hls_segment_filename', os.path.join(self.hls_dir, 'seg-%06d.ts'),
                    os.path.join(self.hls_dir, 'live.m3u8')]
            self.ffmpeg_proc = subprocess.Popen(cmd, cwd=HERE)
            self.serve_proc = subprocess.Popen(
                [sys.executable, SERVE_HLS, '--port', str(self.hls_port),
                 '--bind', '0.0.0.0', '--directory', self.hls_dir], cwd=HERE)
            self.status_data['streaming'] = True
            return True

    def wait_segments(self, timeout=20):
        m3u8 = os.path.join(self.hls_dir or '', 'live.m3u8')
        deadline = now() + timeout
        while now() < deadline:
            try:
                text = open(m3u8, encoding='utf-8').read()
                if text.count('#EXTINF:') >= 3:
                    return True
            except OSError:
                pass
            time.sleep(0.3)
        return False

    # ---- 控制面 ----
    def cast_to(self, dev):
        """切换投屏目标：探活 -> 确保 HLS -> Stop(old) -> SetURI -> Play(new)。"""
        online, _state = d.probe_device(dev['controlURL'])
        if not online:
            return False, '设备不在线（GetTransportInfo 无响应），请重新扫描'
        if not self.start_stream():
            return False, 'HLS 启动失败'
        if not self.wait_segments():
            return False, 'HLS 切片未就绪'
        try:
            urllib.request.urlopen(self.stream_url, timeout=5).status
        except Exception as exc:
            return False, '本地流自检失败: %s' % exc

        control = dev['controlURL']
        # 先停旧目标（若与当前目标不同）
        with self.lock:
            old = self.target
        if old and old.get('udn') != dev.get('udn'):
            try:
                d.soap_call(old['controlURL'], d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
            except Exception:
                pass

        d.soap_call(control, d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
        status, raw = d.soap_call(control, d.AVTRANSPORT, 'SetAVTransportURI', {
            'InstanceID': '0', 'CurrentURI': self.stream_url,
            'CurrentURIMetaData': d.didl_metadata(self.stream_url, 'application/x-mpegURL', 'DLNA Console'),
        })
        if status >= 400:
            return False, 'SetAVTransportURI 失败 HTTP %s: %s' % (status, raw[:300])
        status, raw = d.soap_call(control, d.AVTRANSPORT, 'Play', {'InstanceID': '0', 'Speed': '1'})
        if status >= 400:
            return False, 'Play 失败 HTTP %s: %s' % (status, raw[:300])

        with self.lock:
            self.target = dict(dev)
            self.status_data.update({'target': dev.get('displayName') or dev.get('friendlyName'),
                                     'message': '投屏中', 'first_frame_ms': None, 'tv_http_hits': 0})
        self._start_monitor()
        return True, '已投到 %s' % (dev.get('displayName') or dev.get('friendlyName'))

    def stop_cast(self):
        with self.lock:
            target = self.target
            self.target = None
            self.stop_monitor.set()
            self.status_data.update({'target': None, 'state': 'STOPPED',
                                     'pos_s': None, 'message': '已停止投屏'})
        if target:
            try:
                d.soap_call(target['controlURL'], d.AVTRANSPORT, 'Stop', {'InstanceID': '0'})
            except Exception:
                pass
        return True, '已停止'

    def _start_monitor(self):
        if self.monitor_thread and self.monitor_thread.is_alive():
            return
        self.stop_monitor.clear()
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()

    def _monitor_loop(self):
        target_ip = ''
        with self.lock:
            if self.target:
                target_ip = self.target.get('ip', '')
        try:
            with open(ACCESS_LOG, encoding='utf-8', errors='replace') as fh:
                fh.seek(0, os.SEEK_END)
        except OSError:
            pass
        start = now()
        hits = 0
        first_ms = None
        while not self.stop_monitor.wait(1.0):
            with self.lock:
                target = self.target
                if not target:
                    break
                control = target['controlURL']
            try:
                _s, pos_raw = d.soap_call(control, d.AVTRANSPORT, 'GetPositionInfo', {'InstanceID': '0'})
                _s, tr_raw = d.soap_call(control, d.AVTRANSPORT, 'GetTransportInfo', {'InstanceID': '0'})
                rel = d.parse_tag(pos_raw, 'RelTime')
                state = d.parse_tag(tr_raw, 'CurrentTransportState') or ''
                pos = d.parse_time(rel) if rel and rel != 'NOT_IMPLEMENTED' else None
            except Exception:
                state, pos = '', None
            try:
                with open(ACCESS_LOG, encoding='utf-8', errors='replace') as fh:
                    for line in fh:
                        if target_ip and target_ip in line:
                            hits += 1
                            if first_ms is None:
                                first_ms = int((now() - start) * 1000)
            except OSError:
                pass
            with self.lock:
                self.status_data.update({'state': state, 'pos_s': pos,
                                         'tv_http_hits': hits, 'first_frame_ms': first_ms})

    def status(self):
        with self.lock:
            s = dict(self.status_data)
        s['stream_url'] = self.stream_url
        s['source'] = self.source or 'testsrc2+440Hz（测试图）'
        return s

    def set_source(self, path):
        with self.lock:
            self.source = path
            was_casting = self.target is not None
            target = dict(self.target) if self.target else None
            for proc in (self.ffmpeg_proc, self.serve_proc):
                if proc and proc.poll() is None:
                    proc.terminate()
            self.ffmpeg_proc = None
            self.serve_proc = None
            if self.hls_dir:
                shutil.rmtree(self.hls_dir, ignore_errors=True)
            self.hls_dir = None
            self.status_data['streaming'] = False
        if was_casting and target:
            return self.cast_to(target)
        return True, '片源已切换，下次投屏生效'

    def shutdown(self):
        self.stop_cast()
        with self.lock:
            for proc in (self.ffmpeg_proc, self.serve_proc):
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        proc.kill()
            if self.hls_dir:
                shutil.rmtree(self.hls_dir, ignore_errors=True)


class ControlHandler(BaseHTTPRequestHandler):
    registry = None
    session = None

    def log_message(self, fmt, *args):
        sys.stderr.write('[http] %s - %s\n' % (self.address_string(), fmt % args))

    def _send(self, code, body, ctype='application/json; charset=utf-8'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        data = body.encode('utf-8') if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def _body_json(self):
        try:
            length = int(self.headers.get('Content-Length', 0))
            return json.loads(self.rfile.read(length) or b'{}')
        except Exception:
            return {}

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)
        if path == '/':
            try:
                with open(CONSOLE_HTML, encoding='utf-8') as fh:
                    return self._send(200, fh.read(), 'text/html; charset=utf-8')
            except OSError as exc:
                return self._send(500, {'ok': False, 'error': '页面文件缺失: %s' % exc})
        if path == '/api/devices':
            if qs.get('refresh'):
                self.registry.scan(seconds=int(qs.get('seconds', ['8'])[0]))
            return self._send(200, {
                'ok': True,
                'devices': self.registry.list_devices(),
                'last_udn': self.registry.last_udn,
                'last_scan': self.registry.last_scan,
            })
        if path == '/api/status':
            return self._send(200, {'ok': True, **self.session.status()})
        if path == '/api/source':
            return self._send(200, {'ok': True, 'source': self.session.source})
        return self._send(404, {'ok': False, 'error': 'not found'})

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        body = self._body_json()
        if path == '/api/cast':
            udn = body.get('udn', '')
            dev = self.registry.get(udn)
            if not dev:
                return self._send(404, {'ok': False, 'error': '设备不在注册表中，请先重新扫描'})
            dev['displayName'] = self._display(dev)
            ok, msg = self.session.cast_to(dev)
            if ok:
                self.registry.mark_last(udn)
            return self._send(200 if ok else 502, {'ok': ok, 'message': msg})
        if path == '/api/stop':
            ok, msg = self.session.stop_cast()
            return self._send(200, {'ok': ok, 'message': msg})
        if path == '/api/source':
            p = body.get('path', '')
            if p and not os.path.isfile(p):
                return self._send(400, {'ok': False, 'error': '文件不存在: %s' % p})
            ok, msg = self.session.set_source(p)
            return self._send(200 if ok else 502, {'ok': ok, 'message': msg})
        return self._send(404, {'ok': False, 'error': 'not found'})

    def _display(self, dev):
        with self.registry.lock:
            return d.display_name(dev, list(self.registry.devices.values()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', type=int, default=8090, help='控制台 HTTP 端口')
    ap.add_argument('--hls-port', type=int, default=8080, help='HLS 流 HTTP 端口')
    ap.add_argument('--local-ip', help='本机局域网 IP（默认自动检测）')
    ap.add_argument('--source', help='片源文件；不给则用测试图')
    ap.add_argument('--no-initial-scan', action='store_true')
    args = ap.parse_args()

    if not os.path.isfile(FFMPEG):
        print('ERROR ffmpeg 不存在: %s' % FFMPEG, file=sys.stderr)
        return 2

    # 本机 IP：用默认网关出口探测，不依赖具体电视 IP
    if not args.local_ip:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(('223.5.5.5', 80))
            args.local_ip = s.getsockname()[0]
        except OSError:
            args.local_ip = '127.0.0.1'
        finally:
            s.close()

    registry = DeviceRegistry(args.local_ip)
    session = CastSession(args)
    ControlHandler.registry = registry
    ControlHandler.session = session

    server = ThreadingHTTPServer(('0.0.0.0', args.port), ControlHandler)
    atexit.register(session.shutdown)

    def on_signal(_sig, _frame):
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    print('本机 IP=%s  控制台 http://localhost:%d  HLS :%d' % (
        args.local_ip, args.port, args.hls_port), file=sys.stderr)
    if not args.no_initial_scan:
        n = registry.scan(seconds=8)
        print('初始扫描发现 %d 台设备' % n, file=sys.stderr)
        for dev in registry.list_devices():
            print('  - %s  udn=%s  ip=%s' % (dev['displayName'], dev['udn'], dev['ip']), file=sys.stderr)

    try:
        server.serve_forever()
    finally:
        session.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
