"""ETH mode test for two bridges plugged into the same Windows PC.

Windows never puts a packet on the wire when the destination is one of its own addresses,
so a plain "ping" between the two adapters proves nothing here. This script uses Npcap
(wpcap.dll, no other dependency) instead: it sends raw Ethernet frames out of one bridge's
network adapter and captures what comes out of the other one.

    python eth_test.py                 ARP + packets of several sizes in both directions
    python eth_test.py --count 200 --size 1200 --gap 0     throughput (e.g. with AT+FAST=1)

Run eth_test.ps1 for the complete sequence (mode switch, DHCP, address change, 2.4G).
"""
import argparse
import ctypes as C
import json
import os
import struct
import subprocess
import sys
import time


def adapters():
    ps = ("Get-NetAdapter | Where-Object { $_.MacAddress -match '^02-4C' -and $_.Status -eq 'Up' } | ForEach-Object { "
          "$ip = (Get-NetIPAddress -InterfaceIndex $_.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1); "
          "[pscustomobject]@{ name=$_.Name; guid=$_.InterfaceGuid; mac=$_.MacAddress; ip=$ip.IPAddress; prefix=$ip.PrefixLength } } | ConvertTo-Json")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True).stdout.decode("utf-8", "replace").strip()
    if not out:
        return []
    j = json.loads(out)
    return j if isinstance(j, list) else [j]


class Pcap:
    def __init__(self):
        os.add_dll_directory(r"C:\Windows\System32\Npcap")
        self.l = C.WinDLL("wpcap.dll")
        self.l.pcap_open_live.restype = C.c_void_p
        self.l.pcap_open_live.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_char_p]
        self.l.pcap_sendpacket.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
        self.l.pcap_next_ex.argtypes = [C.c_void_p, C.POINTER(C.c_void_p), C.POINTER(C.POINTER(C.c_ubyte))]
        self.l.pcap_close.argtypes = [C.c_void_p]
        self.l.pcap_setmintocopy.argtypes = [C.c_void_p, C.c_int]

    def open(self, guid):
        err = C.create_string_buffer(256)
        h = self.l.pcap_open_live(("\\Device\\NPF_" + guid).encode(), 65536, 1, 1, err)
        if not h:
            raise RuntimeError("pcap_open_live %s: %s" % (guid, err.value.decode(errors="replace")))
        self.l.pcap_setmintocopy(h, 0)
        return h

    def send(self, h, frame):
        if self.l.pcap_sendpacket(h, frame, len(frame)) != 0:
            raise RuntimeError("pcap_sendpacket failed")

    def recv(self, h):
        """one captured frame or None"""
        hdr = C.c_void_p()
        data = C.POINTER(C.c_ubyte)()
        r = self.l.pcap_next_ex(h, C.byref(hdr), C.byref(data))
        if r != 1:
            return None
        caplen = struct.unpack_from("<I", C.string_at(hdr.value, 16), 8)[0]
        return C.string_at(data, caplen)


def mac(s):
    return bytes(int(x, 16) for x in s.replace(":", "-").split("-"))


def ip4(s):
    return bytes(int(x) for x in s.split("."))


def csum(b):
    if len(b) & 1:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def udp_frame(dst_mac, src_mac, src_ip, dst_ip, ident, size):
    """Ethernet + IPv4 + UDP, IP length = size, payload is a recognisable pattern"""
    plen = size - 28
    payload = (b"LBETH%08d" % ident + bytes((ident * 7 + i * 13) & 0xFF for i in range(plen)))[:plen]
    udp = struct.pack("!HHHH", 40000, 40001, 8 + plen, 0) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), ident & 0xFFFF, 0, 64, 17, 0, ip4(src_ip), ip4(dst_ip))
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return dst_mac + src_mac + b"\x08\x00" + ip + udp


def gate_mac(host_mac):
    return bytes([0x02, 0x4D]) + host_mac[2:]


def test_arp(pc, h, a, peer_ip):
    """the bridge answers ARP for the other host itself"""
    m = mac(a["mac"])
    req = b"\xff" * 6 + m + b"\x08\x06" + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1) + m + ip4(a["ip"]) + b"\0" * 6 + ip4(peer_ip)
    pc.send(h, req + b"\0" * 18)
    end = time.time() + 1.5
    while time.time() < end:
        f = pc.recv(h)
        if f and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02" and f[28:32] == ip4(peer_ip):
            return f[22:28]
    return None


def run_dir(pc, hs, src, dst, sizes, count, gap, timeout):
    """send count packets of each size from src to dst; returns (sent, got, bad, bytes, seconds, latencies)"""
    hsrc, hdst = hs[src["guid"]], hs[dst["guid"]]
    smac, dmac = mac(src["mac"]), mac(dst["mac"])
    while pc.recv(hdst):
        pass
    want = {}
    ident = int(time.time() * 10) % 50000
    t0 = time.time()
    lat = []
    got = bad = nbytes = 0
    seq = [(s, i) for s in sizes for i in range(count)]

    def poll():
        nonlocal got, bad, nbytes
        while True:
            f = pc.recv(hdst)
            if not f:
                return
            # what the far bridge hands to its host: IP packet from our host's address
            if len(f) < 42 or f[12:14] != b"\x08\x00" or f[26:30] != ip4(src["ip"]) or f[42:47] != b"LBETH":
                continue
            ipid = struct.unpack_from("!H", f, 18)[0]
            w = want.pop(ipid, None)
            if w is None:
                continue
            exp, ts = w
            if f[14:] == exp[14:14 + len(f) - 14] and len(f) >= len(exp) and f[:6] in (dmac, b"\xff" * 6) and f[6:12] == gate_mac(dmac):
                got += 1
                nbytes += len(exp) - 14
                lat.append(time.time() - ts)
            else:
                bad += 1

    for size, i in seq:
        ident = (ident + 1) & 0xFFFF
        fr = udp_frame(gate_mac(smac), smac, src["ip"], dst["ip"], ident, size)
        want[ident] = (fr, time.time())
        pc.send(hsrc, fr)
        end = time.time() + gap
        while True:
            poll()
            if time.time() >= end:
                break
            time.sleep(0.001)
    end = time.time() + timeout
    while want and time.time() < end:
        poll()
        time.sleep(0.002)
    return len(seq), got, bad, nbytes, (max(lat) + 0 if lat else 0), lat, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--size", type=int, nargs="*", default=[60, 200, 600, 1400])
    ap.add_argument("--gap", type=float, default=-1, help="seconds between packets (default: by size, for LoRa)")
    ap.add_argument("--timeout", type=float, default=25)
    a = ap.parse_args()
    ads = [x for x in adapters() if x.get("ip")]
    if len(ads) != 2:
        print("FAIL: need two bridge network adapters with an address, found %d: %s" % (len(ads), ads))
        return 2
    ads.sort(key=lambda x: x["ip"])
    for x in ads:
        print("adapter %-12s %s  %s/%s" % (x["name"], x["mac"], x["ip"], x["prefix"]))
    pc = Pcap()
    hs = {x["guid"]: pc.open(x["guid"]) for x in ads}
    ok = True
    for x, y in ((ads[0], ads[1]), (ads[1], ads[0])):
        g = test_arp(pc, hs[x["guid"]], x, y["ip"])
        good = g == gate_mac(mac(x["mac"]))
        ok &= good
        print("ARP  %s asks for %s -> %s  %s" % (x["ip"], y["ip"], g.hex(":") if g else "no answer", "ok" if good else "FAIL"))
    for x, y in ((ads[0], ads[1]), (ads[1], ads[0])):
        for size in a.size:
            gap = a.gap if a.gap >= 0 else 0.5 + size / 150.0
            n, got, bad, nbytes, _, lat, dt = run_dir(pc, hs, x, y, [size], a.count, gap, a.timeout)
            good = got == n and not bad
            ok &= good
            avg = sum(lat) / len(lat) if lat else 0
            print("IP   %s -> %s  %4d bytes x %d : %d intact, %d damaged, latency avg %.2f s max %.2f s, %d B/s  %s"
                  % (x["ip"], y["ip"], size, n, got, bad, avg, max(lat) if lat else 0, nbytes / dt if dt else 0, "ok" if good else "FAIL"))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
