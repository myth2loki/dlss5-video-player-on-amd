#!/usr/bin/env python3
"""Enumerate every UPnP/DLNA device on the LAN and every service endpoint it exposes.

For each device: SSDP discovery -> description.xml -> serviceList -> per-service
SCPD (readable? action list?) and a safe read-only action probe on controlURL.

Usage:
  python3 scan_dlna.py --local-ip 192.168.1.31 --seconds 8
"""
import argparse
import re
import socket
import struct
import sys
import time
import urllib.request
from urllib.parse import urljoin

MSEARCH = (
    'M-SEARCH * HTTP/1.1\r\n'
    'HOST: 239.255.255.250:1900\r\n'
    'MAN: "ssdp:discover"\r\n'
    'MX: 3\r\n'
    'ST: ssdp:all\r\n'
    '\r\n'
).encode('ascii')

SOAP_ENV = 'http://schemas.xmlsoap.org/soap/envelope/'


def log(msg=''):
    print(msg, flush=True)


def discover(local_ip, seconds):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                        struct.pack('4s4s', socket.inet_aton('239.255.255.250'),
                                    socket.inet_aton(local_ip)))
    except OSError:
        pass
    sock.bind(('', 0))
    sock.settimeout(1.0)
    for _ in range(3):
        sock.sendto(MSEARCH, ('239.255.255.250', 1900))
    found = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(8192)
        except socket.timeout:
            continue
        except OSError:
            break
        head = data.decode('utf-8', 'replace')
        def header(name):
            for line in head.split('\r\n'):
                if line.lower().startswith(name.lower() + ':'):
                    return line.split(':', 1)[1].strip()
            return '-'
        loc = header('LOCATION')
        if loc and addr[0] not in found:
            found[addr[0]] = {'location': loc, 'st': header('ST'),
                              'server': header('SERVER'), 'usn': header('USN')}
    sock.close()
    return found


def fetch(url, timeout=6):
    try:
        r = urllib.request.urlopen(url, timeout=timeout)
        return r.status, r.read().decode('utf-8', 'replace')
    except Exception as exc:
        return None, str(exc)


def tag(xml, name):
    m = re.search(r'<(?:\w+:)?%s>(.*?)</(?:\w+:)?%s>' % (name, name), xml, re.S)
    return m.group(1).strip() if m else None


def scan_device(ip, info):
    loc = info['location']
    log('=' * 74)
    log('DEVICE %s' % ip)
    log('  ST:       %s' % info['st'])
    log('  SERVER:   %s' % info['server'])
    log('  LOCATION: %s' % loc)
    log('  USN:      %s' % info['usn'])

    status, xml = fetch(loc)
    if status != 200:
        log('  !! description fetch failed: %s %s' % (status, xml))
        return
    log('  friendlyName: %s' % (tag(xml, 'friendlyName') or '-'))
    log('  manufacturer: %s' % (tag(xml, 'manufacturer') or '-'))
    log('  modelName:    %s' % (tag(xml, 'modelName') or '-'))
    log('  deviceType:   %s' % (tag(xml, 'deviceType') or '-'))

    svcs = re.findall(r'<service>(.*?)</service>', xml, re.S)
    log('  services: %d' % len(svcs))
    base = loc.rsplit('/', 1)[0] + '/'
    for i, svc in enumerate(svcs, 1):
        stype = tag(svc, 'serviceType') or '-'
        sid = tag(svc, 'serviceId') or '-'
        scpd = tag(svc, 'SCPDURL') or ''
        ctrl = tag(svc, 'controlURL') or ''
        evt = tag(svc, 'eventSubURL') or ''
        scpd_url = urljoin(base, scpd) if scpd else '(none)'
        ctrl_url = urljoin(base, ctrl) if ctrl else '(none)'
        evt_url = urljoin(base, evt) if evt else '(none)'
        log('')
        log('  [%d] %s' % (i, stype))
        log('      serviceId : %s' % sid)
        log('      SCPD      : %s' % scpd_url)
        log('      controlURL: %s' % ctrl_url)
        log('      eventSub  : %s' % evt_url)

        if scpd and scpd_url != '(none)':
            s_status, s_xml = fetch(scpd_url)
            if s_status == 200:
                actions = re.findall(r'<action>\s*<name>(.*?)</name>', s_xml, re.S)
                log('      SCPD -> HTTP %s, %d actions: %s' % (
                    s_status, len(actions), ', '.join(actions[:14]) or '(none)'))
            else:
                log('      SCPD -> HTTP %s (%s)' % (s_status, str(s_xml)[:80]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local-ip', default='192.168.1.31')
    ap.add_argument('--seconds', type=int, default=8)
    args = ap.parse_args()

    log('SSDP discovery on %s (window %ds)...' % (args.local_ip, args.seconds))
    devices = discover(args.local_ip, args.seconds)
    if not devices:
        log('NO SSDP RESPONSE')
        return 2
    log('found %d device(s)' % len(devices))
    for ip in sorted(devices, key=lambda x: tuple(int(p) for p in x.split('.'))):
        scan_device(ip, devices[ip])
    log('=' * 74)
    log('done.')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
