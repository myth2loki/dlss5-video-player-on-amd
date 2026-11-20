#!/usr/bin/env python3
"""SSDP M-SEARCH discovery for UPnP/DLNA devices on the local LAN.

Prints one block per responding device: IP, ST, LOCATION (description.xml URL), SERVER.
Usage: python3 ssdp_discover.py [--local-ip 192.168.1.31] [--seconds 8]
"""
import argparse
import socket
import struct
import sys
import time

MSEARCH = (
    'M-SEARCH * HTTP/1.1\r\n'
    'HOST: 239.255.255.250:1900\r\n'
    'MAN: "ssdp:discover"\r\n'
    'MX: 3\r\n'
    'ST: ssdp:all\r\n'
    '\r\n'
).encode('ascii')


def discover(local_ip: str, seconds: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(
            socket.IPPROTO_IP,
            socket.IP_ADD_MEMBERSHIP,
            struct.pack('4s4s', socket.inet_aton('239.255.255.250'), socket.inet_aton(local_ip)),
        )
    except OSError as exc:
        print('WARN: IP_ADD_MEMBERSHIP failed: %s' % exc, file=sys.stderr)
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
        if addr[0] in found:
            continue

        def header(name):
            for line in head.split('\r\n'):
                if line.lower().startswith(name.lower() + ':'):
                    return line.split(':', 1)[1].strip()
            return '-'

        found[addr[0]] = {
            'st': header('ST'),
            'location': header('LOCATION'),
            'server': header('SERVER'),
            'usn': header('USN'),
        }
    sock.close()
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--local-ip', default='192.168.1.31')
    ap.add_argument('--seconds', type=int, default=8)
    args = ap.parse_args()

    devices = discover(args.local_ip, args.seconds)
    if not devices:
        print('NO SSDP RESPONSE')
        return
    for ip in sorted(devices, key=lambda x: tuple(int(p) for p in x.split('.'))):
        d = devices[ip]
        print('IP=%s' % ip)
        print('   ST:       %s' % d['st'])
        print('   LOCATION: %s' % d['location'])
        print('   SERVER:   %s' % d['server'])
        print('   USN:      %s' % d['usn'])


if __name__ == '__main__':
    main()
