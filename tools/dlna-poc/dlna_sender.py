#!/usr/bin/env python3
"""Minimal DLNA/UPnP AVTransport sender — push a media URL to a DMR and trace position.

Contrast with cast-poc/cast_sender.js (CastV2/TLS:8009): this talks SOAP/HTTP to
a UPnP MediaRenderer, which is what the China-market Sony X9500H actually exposes
(SMG_CAST_PLAYER, DMR-1.50). No Chromecast, no TLS, no CastV2.

Emits JSONL one row per poll with wall-clock t_ms, transport state and playhead
seconds, suitable for judge_r1.py-style continuity judging.

Usage:
  python3 dlna_sender.py --url http://192.168.1.31:8080/live.m3u8 \
      --mime application/x-mpegURL --seconds 75 --out trace.jsonl
  # or let it discover the DMR via SSDP:
  python3 dlna_sender.py --discover --url ... --out trace.jsonl
"""
import argparse
import json
import re
import socket
import struct
import sys
import time
import urllib.request
from urllib.parse import urlparse, urljoin

SOAP_ENV = 'http://schemas.xmlsoap.org/soap/envelope/'
AVTRANSPORT = 'urn:schemas-upnp-org:service:AVTransport:1'
CM = 'urn:schemas-upnp-org:service:ConnectionManager:1'
MSEARCH = (
    'M-SEARCH * HTTP/1.1\r\n'
    'HOST: 239.255.255.250:1900\r\n'
    'MAN: "ssdp:discover"\r\n'
    'MX: 3\r\n'
    'ST: upnp:rootdevice\r\n'
    '\r\n'
).encode('ascii')


def log(msg):
    print(msg, file=sys.stderr, flush=True)


# ---------- discovery ----------

def ssdp_discover(local_ip, seconds):
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
    locations = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(8192)
        except socket.timeout:
            continue
        except OSError:
            break
        head = data.decode('utf-8', 'replace')
        for line in head.split('\r\n'):
            if line.lower().startswith('location:'):
                loc = line.split(':', 1)[1].strip()
                if loc not in locations:
                    locations.append(loc)
    sock.close()
    return locations


def parse_description(location):
    """Return (friendly_name, control_url_base, avtransport_control_url)."""
    xml = urllib.request.urlopen(location, timeout=8).read().decode('utf-8', 'replace')
    name = re.search(r'<friendlyName>(.*?)</friendlyName>', xml, re.S)
    friendly = name.group(1).strip() if name else 'unknown'
    svc = re.search(
        r'<service>(?:(?!</service>).)*?AVTransport:1(?:(?!</service>).)*?</service>',
        xml, re.S)
    control = None
    if svc:
        m = re.search(r'<controlURL>(.*?)</controlURL>', svc.group(0), re.S)
        if m:
            control = m.group(1).strip()
    base = location.rsplit('/', 1)[0] + '/'
    if not control:
        return friendly, base, base + 'control'
    return friendly, base, urljoin(base, control)


# ---------- SOAP ----------

def soap_call(control_url, service, action, params=None, timeout=8):
    params = params or {}
    inner = ''.join('<%s>%s</%s>' % (k, _xml_escape(v), k) for k, v in params.items())
    body = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="%s"><s:Body>'
        '<u:%s xmlns:u="%s">%s</u:%s>'
        '</s:Body></s:Envelope>'
    ) % (SOAP_ENV, action, service, inner, action)
    req = urllib.request.Request(
        control_url,
        data=body.encode('utf-8'),
        headers={
            'Content-Type': 'text/xml; charset=utf-8',
            'SOAPACTION': '"%s#%s"' % (service, action),
            'User-Agent': 'dlna-poc/1.0',
        },
    )
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        raw = resp.read().decode('utf-8', 'replace')
        return resp.status, raw
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode('utf-8', 'replace')


def _xml_escape(text):
    return (str(text).replace('&', '&amp;').replace('<', '&lt;')
            .replace('>', '&gt;').replace('"', '&quot;').replace("'", '&apos;'))


def didl_metadata(url, mime, title='POC Live'):
    protocol = 'http-get:*:%s:*' % mime
    res = '<res protocolInfo="%s">%s</res>' % (_xml_escape(protocol), _xml_escape(url))
    return (
        '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
        'xmlns:dc="http://purl.org/dc/elements/1.1/" '
        'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
        '<item id="0" parentID="-1" restricted="1">'
        '<dc:title>%s</dc:title>'
        '<upnp:class>object.item.videoItem</upnp:class>'
        '%s</item></DIDL-Lite>'
    ) % (_xml_escape(title), res)


def parse_time(value):
    """UPnP clock H:MM:SS(.frac) -> seconds float."""
    if not value or ':' not in value:
        return None
    try:
        parts = value.split(':')
        if len(parts) == 3:
            h, m, s = parts
        elif len(parts) == 2:
            h, m, s = '0', parts[0], parts[1]
        else:
            return None
        return int(h) * 3600 + int(m) * 60 + float(s)
    except ValueError:
        return None


def parse_tag(raw, tag):
    m = re.search(r'<%s>(.*?)</%s>' % (tag, tag), raw, re.S)
    return m.group(1).strip() if m else None


# ---------- structured device discovery (identity = UDN) ----------

def parse_description_full(location):
    """Parse a device description into a structured dict.

    Identity is the <UDN> (stable across DHCP/IP changes). friendlyName is display
    only, ip is connection-only — neither is used as an identity key.
    """
    xml = urllib.request.urlopen(location, timeout=8).read().decode('utf-8', 'replace')

    def one(name):
        m = re.search(r'<(?:[\w-]+:)?%s>(.*?)</(?:[\w-]+:)?%s>' % (name, name), xml, re.S)
        return m.group(1).strip() if m else ''

    udn = one('UDN') or ''
    friendly = one('friendlyName') or 'unknown'
    model = one('modelName') or ''
    manufacturer = one('manufacturer') or ''
    device_type = one('deviceType') or ''

    svc = re.search(
        r'<service>(?:(?!</service>).)*?AVTransport:1(?:(?!</service>).)*?</service>',
        xml, re.S)
    control = ''
    if svc:
        m = re.search(r'<(?:[\w-]+:)?controlURL>(.*?)</(?:[\w-]+:)?controlURL>', svc.group(0), re.S)
        if m:
            control = m.group(1).strip()
    base = location.rsplit('/', 1)[0] + '/'
    control_url = urljoin(base, control) if control else base + 'control'
    # A device is a real DMR only if it declares AVTransport:1. Sony exposes a
    # sibling service on :7084 with only ScalarWebAPI/DIAL/IRCC — that is a
    # DIAL discovery endpoint (used by B站/YouTube to launch the TV app), NOT a
    # DMR, and cannot accept our SetAVTransportURI. Record this so UI can label
    # it correctly instead of misreporting it as an "offline DMR".
    has_avtransport = bool(svc)

    ip = urlparse(location).hostname or ''
    return {
        'udn': udn,
        'friendlyName': friendly,
        'modelName': model,
        'manufacturer': manufacturer,
        'deviceType': device_type,
        'location': location,
        'controlURL': control_url,
        'ip': ip,
        'hasAVTransport': has_avtransport,
    }


def discover_devices(local_ip, seconds=8, st='upnp:rootdevice'):
    """SSDP discover + fetch descriptions -> deduped device list keyed by UDN.

    Dedupe by UDN (stable identity), not by LOCATION/IP (both can change). Same
    device answering multiple STs collapses to one entry.
    """
    msearch = (
        'M-SEARCH * HTTP/1.1\r\n'
        'HOST: 239.255.255.250:1900\r\n'
        'MAN: "ssdp:discover"\r\n'
        'MX: 3\r\n'
        'ST: %s\r\n'
        '\r\n'
    ).encode('ascii') % st.encode('ascii')

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
        sock.sendto(msearch, ('239.255.255.250', 1900))
    locations = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data, _addr = sock.recvfrom(8192)
        except (socket.timeout, OSError):
            continue
        head = data.decode('utf-8', 'replace')
        for line in head.split('\r\n'):
            if line.lower().startswith('location:'):
                loc = line.split(':', 1)[1].strip()
                if loc and loc not in locations:
                    locations.append(loc)
    sock.close()

    devices = {}
    for loc in locations:
        try:
            dev = parse_description_full(loc)
        except Exception:
            continue
        key = dev['udn'] or dev['location']
        if key not in devices:
            dev['lastSeen'] = time.time()
            dev['online'] = True
            devices[key] = dev
    return list(devices.values())


def probe_device(control_url, timeout=3):
    """Endpoint-reachability probe -> (reachable, detail).

    IMPORTANT (fixed 2026-11-20): a DMR with no active playback instance legitimately
    answers GetTransportInfo with 501 "Action Failed" (UPnP error 501). That is a
    VALID SOAP response — it proves the endpoint is alive and speaking UPnP, NOT
    that the device is offline or uncastable. Treating status != 200 as offline
    was a broken metric that mislabelled the Sony native DMR on KD-55X9500H.

    The only real "can it play our URL" test is SetAVTransportURI inside cast_to;
    probing must not exercise it (side effect: hijacks whatever is playing).

      reachable=True   HTTP SOAP answer arrived (200 with state, or 500+fault)
      reachable=False  timeout / connection refused / no HTTP response
    """
    try:
        status, raw = soap_call(control_url, AVTRANSPORT, 'GetTransportInfo',
                                {'InstanceID': '0'}, timeout=timeout)
    except Exception:
        return False, 'no-response'
    if status and status < 600:
        state = parse_tag(raw, 'CurrentTransportState') or ''
        if state:
            return True, state
        ec = re.search(r'<errorCode>(\d+)', raw)
        return True, ('UPnP-error-%s' % ec.group(1)) if ec else ('HTTP-%s' % status)
    return False, 'HTTP-%s' % status


def display_name(dev, all_devices):
    """Friendly name with a stable UDN suffix when names collide."""
    name = dev.get('friendlyName') or dev.get('ip') or 'unknown'
    collisions = sum(1 for d in all_devices
                     if (d.get('friendlyName') or d.get('ip')) == name)
    if collisions > 1 and dev.get('udn'):
        tail = re.sub(r'^uuid:', '', dev['udn'])[-5:]
        return '%s (%s)' % (name, tail)
    return name


# ---------- main flow ----------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--discover', action='store_true', help='SSDP-discover the DMR instead of --host')
    ap.add_argument('--local-ip', default='192.168.1.31')
    ap.add_argument('--desc', help='description.xml URL (skip SSDP)')
    ap.add_argument('--control', help='AVTransport control URL (skip description parse)')
    ap.add_argument('--url', required=True, help='media URL to push')
    ap.add_argument('--mime', default='video/mp2t', help='MIME for DIDL protocolInfo')
    ap.add_argument('--title', default='POC Live')
    ap.add_argument('--seconds', type=int, default=75)
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--out', default='dlna-trace.jsonl')
    args = ap.parse_args()

    control = args.control
    friendly = 'DMR'
    if not control:
        if args.desc:
            location = args.desc
        elif args.discover:
            log('SSDP discovering DMR (local ip %s)...' % args.local_ip)
            locs = ssdp_discover(args.local_ip, 10)
            if not locs:
                log('ERROR no UPnP device discovered')
                return 2
            location = next((l for l in locs if '192.168.1.24' in l), locs[0])
            log('  picked %s' % location)
        else:
            log('ERROR provide --discover, --desc or --control')
            return 2
        friendly, base, control = parse_description(location)
        log('DMR=%s control=%s' % (friendly, control))

    log('Pushing %s (mime=%s)' % (args.url, args.mime))
    status, raw = soap_call(control, AVTRANSPORT, 'SetAVTransportURI', {
        'InstanceID': '0',
        'CurrentURI': args.url,
        'CurrentURIMetaData': didl_metadata(args.url, args.mime, args.title),
    })
    log('SetAVTransportURI -> HTTP %s' % status)
    if status >= 400:
        log(raw[:800])
        return 3

    status, raw = soap_call(control, AVTRANSPORT, 'Play', {'InstanceID': '0', 'Speed': '1'})
    log('Play -> HTTP %s' % status)
    if status >= 400:
        log(raw[:800])
        return 4

    start = time.time()
    end = start + args.seconds
    rows = []
    first_ms = None
    with open(args.out, 'w', encoding='utf-8') as fh:
        while time.time() < end:
            now = time.time()
            t_ms = int((now - start) * 1000)
            s_pos, pos_raw = soap_call(control, AVTRANSPORT, 'GetPositionInfo',
                                       {'InstanceID': '0'})
            s_tr, tr_raw = soap_call(control, AVTRANSPORT, 'GetTransportInfo',
                                     {'InstanceID': '0'})
            rel = parse_tag(pos_raw, 'RelTime')
            abs_t = parse_tag(pos_raw, 'AbsTime')
            state = parse_tag(tr_raw, 'CurrentTransportState')
            pos_s = parse_time(rel if rel and rel != 'NOT_IMPLEMENTED' else abs_t)
            if pos_s is not None and first_ms is None and pos_s > 0:
                first_ms = t_ms
            row = {
                't_ms': t_ms,
                'state': state,
                'rel_time': rel,
                'pos_s': pos_s,
                'http': [s_pos, s_tr],
            }
            rows.append(row)
            fh.write(json.dumps(row, ensure_ascii=False) + '\n')
            fh.flush()
            log('t=%5dms state=%-10s pos=%s' % (t_ms, state, rel))
            time.sleep(max(0, args.interval - (time.time() - now)))

    log('WROTE %s rows=%d first_frame_ms=%s' % (args.out, len(rows), first_ms))
    # best-effort stop so the DMR does not keep pulling after the test
    soap_call(control, AVTRANSPORT, 'Stop', {'InstanceID': '0'})
    log('Stopped.')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
