#!/usr/bin/env python3
"""
tftp_recv.py — minimal write-only TFTP server for pulling backup images off the
switch (busybox `tftp -p` uploads to it). No root needed (high UDP port).
Negotiates blksize/tsize (RFC 2347/2348) for fast large transfers.

Usage:  tftp_recv.py [--dir DIR] [--port 6969]
On the switch:
  tftp -p -b 32768 -l /path/local -r NAME 10.1.1.30 6969
"""
import os, sys, socket, struct, argparse, time

OP_WRQ=2; OP_DATA=3; OP_ACK=4; OP_ERR=5; OP_OACK=6

def parse_wrq(data):
    # opcode(2) filename\0 mode\0 [opt\0 val\0]...
    parts = data[2:].split(b"\x00")
    fname = parts[0].decode("latin-1")
    mode = parts[1].decode("latin-1") if len(parts) > 1 else "octet"
    opts = {}
    i = 2
    while i + 1 < len(parts) and parts[i]:
        opts[parts[i].decode("latin-1").lower()] = parts[i+1].decode("latin-1")
        i += 2
    return fname, mode, opts

def recv_one(main, outdir):
    data, cli = main.recvfrom(65535)
    if not data or data[1] != OP_WRQ:
        return
    fname, mode, opts = parse_wrq(data)
    safe = os.path.basename(fname) or "upload.bin"
    path = os.path.join(outdir, safe)
    print(f"[WRQ] {cli[0]}:{cli[1]} -> {safe} mode={mode} opts={opts}", flush=True)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # new TID
    s.bind(("", 0)); s.settimeout(15)

    blksize = 512
    oack = {}
    if "blksize" in opts:
        blksize = max(8, min(int(opts["blksize"]), 65464)); oack["blksize"] = str(blksize)
    if "tsize" in opts:
        oack["tsize"] = opts["tsize"]
    if "timeout" in opts:
        oack["timeout"] = opts["timeout"]

    f = open(path, "wb")
    total = 0
    try:
        if oack:
            pkt = struct.pack("!H", OP_OACK) + b"".join(
                k.encode()+b"\x00"+v.encode()+b"\x00" for k, v in oack.items())
            s.sendto(pkt, cli)
            expected = 1
        else:
            s.sendto(struct.pack("!HH", OP_ACK, 0), cli)  # ACK block 0
            expected = 1
        while True:
            try:
                pkt, _ = s.recvfrom(blksize + 4)
            except socket.timeout:
                print("  ! timeout", flush=True); break
            op = struct.unpack("!H", pkt[:2])[0]
            if op == OP_ERR:
                print("  ! client error:", pkt[4:].decode("latin-1","replace")); break
            if op != OP_DATA:
                continue
            blk = struct.unpack("!H", pkt[2:4])[0]
            if blk == (expected & 0xFFFF):
                chunk = pkt[4:]
                f.write(chunk); total += len(chunk)
                s.sendto(struct.pack("!HH", OP_ACK, blk), cli)
                expected += 1
                if len(chunk) < blksize:
                    print(f"  done: {total} bytes -> {path}", flush=True); break
            else:
                # re-ACK last good block
                s.sendto(struct.pack("!HH", OP_ACK, (expected-1) & 0xFFFF), cli)
    finally:
        f.close(); s.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".")
    ap.add_argument("--port", type=int, default=6969)
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    main_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    main_sock.bind(("", a.port))
    print(f"TFTP write-server on udp/{a.port}, saving to {os.path.abspath(a.dir)}", flush=True)
    while True:
        try:
            recv_one(main_sock, a.dir)
        except Exception as e:
            print("  ! error:", e, flush=True)

if __name__ == "__main__":
    main()
