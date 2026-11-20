#!/usr/bin/env python3
"""Try multiple SetAVTransportURI variants to find one the SMG CAST DMR accepts.

For each variant: Stop -> SetAVTransportURI -> Play -> poll GetTransportInfo 6s.
Success = transport state leaves STOPPED and/or HTTP server sees a GET from the TV.
"""
import re
import sys
import time
import urllib.request

C = 'http://192.168.1.24:63636/control'
S = 'urn:schemas-upnp-org:service:AVTransport:1'
URL = 'http://192.168.1.31:8080/poc-60s.ts'


def esc(t):
    return str(t).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;').replace("'", '&apos;')


def call(action, params):
    inner = ''.join('<%s>%s</%s>' % (k, v, k) for k, v in params.items())
    body = ('<?xml version="1.0" encoding="utf-8"?>'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><u:%s xmlns:u="%s">%s</u:%s></s:Body></s:Envelope>'
            % (action, S, inner, action))
    req = urllib.request.Request(C, data=body.encode(), headers={
        'Content-Type': 'text/xml; charset=utf-8',
        'SOAPACTION': '"%s#%s"' % (S, action),
        'User-Agent': 'poc',
    })
    try:
        r = urllib.request.urlopen(req, timeout=8)
        return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')


def state():
    _, raw = call('GetTransportInfo', {'InstanceID': '0'})
    m = re.search(r'<CurrentTransportState>(.*?)</CurrentTransportState>', raw)
    return m.group(1) if m else '?'


DIDL_TPL = (
    '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
    'xmlns:dc="http://purl.org/dc/elements/1.1/" '
    'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
    '<item id="0" parentID="-1" restricted="1">'
    '<dc:title>POC</dc:title>'
    '<upnp:class>object.item.videoItem</upnp:class>'
    '<res protocolInfo="%s">%s</res>'
    '</item></DIDL-Lite>'
)

VARIANTS = [
    ('empty-meta', ''),
    ('mp2t', DIDL_TPL % ('http-get:*:video/mp2t:*', URL)),
    ('mpeg', DIDL_TPL % ('http-get:*:video/mpeg:*', URL)),
    ('octet', DIDL_TPL % ('http-get:*:application/octet-stream:*', URL)),
    ('mp4', DIDL_TPL % ('http-get:*:video/mp4:*', URL)),
    ('star', DIDL_TPL % ('http-get:*:*:*', URL)),
    ('no-protocol', '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"><item id="0" parentID="-1" restricted="1"><res>' + URL + '</res></item></DIDL-Lite>'),
]

for name, meta in VARIANTS:
    call('Stop', {'InstanceID': '0'})
    params = {'InstanceID': '0', 'CurrentURI': URL, 'CurrentURIMetaData': esc(meta)}
    st, raw = call('SetAVTransportURI', params)
    st2, _ = call('Play', {'InstanceID': '0', 'Speed': '1'})
    time.sleep(0.6)
    s = state()
    print('%-14s set=%s play=%s state_after=%s' % (name, st, st2, s), flush=True)
    if s not in ('STOPPED', '?'):
        print('  >>> VARIANT ACCEPTED:', name)
        break
call('Stop', {'InstanceID': '0'})
