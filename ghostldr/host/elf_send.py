#!/usr/bin/env python3
"""Send an ELF to elfldr (9021) and print matching klog lines for a while."""
import socket, sys, time

def main() -> int:
    from console import console_ip
    host = console_ip()
    path, grep, wait = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else ""), float(sys.argv[3]) if len(sys.argv) > 3 else 6
    data = open(path, "rb").read()
    k = socket.create_connection((host, 3232), timeout=3)
    k.settimeout(0.2)
    t = time.time()
    while time.time() - t < 1.5:
        try:
            if not k.recv(65536):
                break
        except socket.timeout:
            break
    with socket.create_connection((host, 9021), timeout=10) as s:
        s.sendall(data)
        s.shutdown(socket.SHUT_WR)
    buf = b""
    t = time.time()
    while time.time() - t < wait:
        try:
            c = k.recv(65536)
            if not c:
                break
            buf += c
        except socket.timeout:
            pass
    for line in buf.decode("utf-8", "replace").splitlines():
        if not grep or any(g in line for g in grep.split("|")):
            print(line[:300])
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
