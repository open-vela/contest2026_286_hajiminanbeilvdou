#!/usr/bin/env python3
"""Ask a specific DNS server a question, the way the device would.

The device resolves through 114.114.114.114 (CONFIG_NETDB_DNSSERVER_IPv4ADDR
0x72727272). The host does not -- it uses the WSL/Windows resolver -- so a
host-side lookup succeeding says nothing about whether the device can resolve
anything at all. This queries the device's server directly.

    python3 dns_test.py [server] [name]
"""
import socket
import struct
import sys

server = sys.argv[1] if len(sys.argv) > 1 else "114.114.114.114"
name = sys.argv[2] if len(sys.argv) > 2 else "api.deepseek.com"

# Minimal A query, recursion desired.
q = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
for label in name.split("."):
    q += bytes([len(label)]) + label.encode()
q += b"\x00\x00\x01\x00\x01"

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(6)
try:
    s.sendto(q, (server, 53))
    data, _ = s.recvfrom(512)
except Exception as exc:                      # noqa: BLE001
    print("FAIL  %s did not answer: %s" % (server, exc))
    sys.exit(1)

answers = struct.unpack("!H", data[6:8])[0]
print("OK    %s answered, %d answer(s)" % (server, answers))
if answers:
    # Last four bytes of a single-A reply are the address.
    print("      %s -> %d.%d.%d.%d" % (name, *data[-4:]))
