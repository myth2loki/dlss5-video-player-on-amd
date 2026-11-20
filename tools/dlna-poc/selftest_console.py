#!/usr/bin/env python3
"""控制台自检：逻辑层(伪设备消歧/TTL) + API 层(真机拉流)。

用法（需先启动 cast_controller.py）：
  python3 selftest_console.py            # 跑逻辑层 + API 层（真机）
  python3 selftest_console.py --logic-only  # 只跑逻辑层，不需服务
"""
import argparse
import json
import sys
import time
import urllib.request

import dlna_sender as d
import cast_controller as cc

BASE = 'http://127.0.0.1:8090'


def api(path, body=None):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(BASE + path, data=data, headers=headers)
    return json.loads(urllib.request.urlopen(req, timeout=30).read().decode('utf-8'))


def logic_tests():
    print('=== 逻辑层：伪设备消歧 / TTL / 身份键 ===', flush=True)
    ok = True

    # 两台同名设备的伪注册，UDN 不同
    fake = [
        {'udn': 'uuid:SMG_CAST_PLAYER_UPNP_2_0_f57f0cff2cb48a3a',
         'friendlyName': 'SMG投屏24', 'ip': '192.168.1.24',
         'controlURL': 'http://192.168.1.24:63636/control', 'modelName': 'SMG投屏助手'},
        {'udn': 'uuid:SMG_CAST_PLAYER_UPNP_2_0_deadbeef01',
         'friendlyName': 'SMG投屏24', 'ip': '192.168.1.99',
         'controlURL': 'http://192.168.1.99:63636/control', 'modelName': 'SMG投屏助手'},
    ]
    n1 = d.display_name(fake[0], fake)
    n2 = d.display_name(fake[1], fake)
    print('  同名消歧:', n1, '|', n2)
    distinct = n1 != n2 and n1.endswith('48a3a)') and n2.endswith('eef01)')
    print('  ', 'PASS' if distinct else 'FAIL', '- 同名两台用 UDN 尾号区分')
    ok &= distinct

    # 注册表 identity=UDN：两台同名应各占一条
    reg = cc.DeviceRegistry('192.168.1.31')
    orig = d.discover_devices
    d.discover_devices = lambda *a, **k: [dict(x) for x in fake]
    try:
        reg.scan(seconds=0)
    finally:
        d.discover_devices = orig
    count = len(reg.list_devices())
    print('  注册表条数:', count)
    ok &= (count == 2)
    print('  ', 'PASS' if count == 2 else 'FAIL', '- identity=UDN，同名不合并')

    # 探活判据回归：501 SOAP fault 必须判为"可达"（不是离线）。
    # 索尼原生 DMR 在无播放实例时合法返回 501，坏判据会把它误标离线。
    class _FaultResp:
        status = 500
        def read(self):
            return (b'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                    b'<s:Body><s:Fault><detail><UPnPError><errorCode>501</errorCode>'
                    b'</UPnPError></detail></s:Fault></s:Body></s:Envelope>')
    orig_open = d.urllib.request.urlopen
    d.urllib.request.urlopen = lambda *a, **k: _FaultResp()
    try:
        reach, detail = d.probe_device('http://fake/control')
    finally:
        d.urllib.request.urlopen = orig_open
    fault_ok = (reach is True and detail == 'UPnP-error-501')
    print('  501-fault 判可达:', 'PASS' if fault_ok else 'FAIL', '- detail=%s' % detail)
    ok &= fault_ok

    # TTL 离线标记
    for dev in reg.devices.values():
        dev['lastSeen'] = time.time() - (cc.TTL_SECONDS + 5)
    d.discover_devices = lambda *a, **k: []
    try:
        reg.scan(seconds=0)
    finally:
        d.discover_devices = orig
    states = [x['online'] for x in reg.list_devices()]
    ttl_ok = (states == [False, False])
    print('  ', 'PASS' if ttl_ok else 'FAIL', '- TTL 超期标离线但仍保留 (states=%s)' % states)
    ok &= ttl_ok

    print('=== 逻辑层结果:', 'PASS' if ok else 'FAIL', '===', flush=True)
    return ok


def api_tests():
    print('=== API 层：真机扫描 / 投屏 / 拉流 / 重扫 ===', flush=True)
    ok = True

    # 1) 页面托管
    html = urllib.request.urlopen(BASE + '/', timeout=10).read().decode('utf-8')
    page_ok = 'DLNA 投屏控制台' in html and '/api/devices' in html
    print('  ', 'PASS' if page_ok else 'FAIL', '- GET / 托管页面')
    ok &= page_ok

    # 2) 强制扫描
    devs = api('/api/devices?refresh=1&seconds=8')
    lst = devs.get('devices', [])
    print('  扫描到', len(lst), '台:')
    for x in lst:
        print('    -', x['displayName'], x['ip'], x['udn'][-12:], 'online=' + str(x['online']))
    tv = next((x for x in lst if x['ip'] == '192.168.1.24'), None)
    scan_ok = tv is not None and tv['udn'].startswith('uuid:')
    print('  ', 'PASS' if scan_ok else 'FAIL', '- 真机在列表且带 UDN')
    ok &= scan_ok
    if not tv:
        print('=== API 层结果: FAIL（无真机）===', flush=True)
        return False

    # 3) 投到真机
    r = api('/api/cast', {'udn': tv['udn']})
    cast_ok = r.get('ok', False)
    print('  cast:', r.get('message'))
    print('  ', 'PASS' if cast_ok else 'FAIL', '- POST /api/cast 探活通过并投屏')
    ok &= cast_ok
    if not cast_ok:
        return False

    # 4) 轮询状态：等 PLAYING + 电视拉流
    print('  轮询播放头 / 拉流证据（最多 20s）…', flush=True)
    playing = False
    hits = 0
    pos = None
    for _ in range(20):
        time.sleep(1)
        s = api('/api/status')
        hits = s.get('tv_http_hits', 0)
        pos = s.get('pos_s')
        if s.get('state') == 'PLAYING' and hits > 0:
            playing = True
            break
    print('    state pos=%s hits=%d' % (pos, hits))
    print('  ', 'PASS' if playing else 'FAIL', '- 电视 PLAYING 且有 GET 拉流证据')
    ok &= playing

    # 5) 停止
    r = api('/api/stop', {})
    print('  ', 'PASS' if r.get('ok') else 'FAIL', '- POST /api/stop')

    # 6) 重新扫描刷新
    devs2 = api('/api/devices?refresh=1&seconds=6')
    rescan_ok = devs2.get('last_scan', 0) > devs.get('last_scan', 0)
    print('  ', 'PASS' if rescan_ok else 'FAIL', '- 重新扫描刷新 last_scan')

    print('=== API 层结果:', 'PASS' if ok else 'FAIL', '===', flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logic-only', action='store_true')
    args = ap.parse_args()
    a = logic_tests()
    if args.logic_only:
        return 0 if a else 1
    try:
        b = api_tests()
    except Exception as exc:
        print('API 层异常:', exc, flush=True)
        b = False
    return 0 if (a and b) else 1


if __name__ == '__main__':
    sys.exit(main())
