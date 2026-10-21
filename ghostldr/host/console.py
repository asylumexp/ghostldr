"""Find the PS5 on the LAN.

DHCP can move the console, so the host tools look it up with the PlayStation
discovery protocol (UDP 9302).
  PS5_HOST     use this address, no lookup
  PS5_HOST_ID  the console's host-id (as `python3 ghostldr/host/console.py --list`
               shows it): find that console wherever it is
  neither      use the only PS5 that answers (more than one is an error)
The last address found is cached in build/ghostldr/console_ip.
"""

from __future__ import annotations

import os
import socket
import sys
import time
from pathlib import Path

CACHE = Path(__file__).resolve().parents[2] / "build/ghostldr/console_ip"
SRCH = b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"


def _field(reply: bytes, name: str) -> str:
    for line in reply.decode(errors="replace").splitlines():
        if line.lower().startswith(name + ":"):
            return line.split(":", 1)[1].strip()
    return ""


def host_id(ip: str, timeout: float = 1.5) -> str:
    """The console's host-id at `ip` ("" if it does not answer discovery)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        try:
            s.sendto(SRCH, (ip, 9302))
            data, _ = s.recvfrom(2048)
        except OSError:
            return ""
    return _field(data, "host-id").upper()


def discover(wait: float = 2.0) -> dict[str, tuple[str, str, str]]:
    """{ip: (host-id, host-name, system-version)} of the PlayStations that answer."""
    found = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.3)
        s.sendto(SRCH, ("255.255.255.255", 9302))
        deadline = time.time() + wait
        while time.time() < deadline:
            try:
                data, (ip, _) = s.recvfrom(2048)
            except socket.timeout:
                continue
            found[ip] = (_field(data, "host-id").upper(), _field(data, "host-name"), _field(data, "system-version"))
    return found


def console_ip() -> str:
    if os.environ.get("PS5_HOST"):
        return os.environ["PS5_HOST"]
    want = os.environ.get("PS5_HOST_ID", "").upper()
    cached = CACHE.read_text().strip() if CACHE.exists() else ""
    if cached and want and host_id(cached) == want:
        return cached
    found = discover()
    if want:
        hits = [ip for ip, info in found.items() if info[0] == want]
    else:
        hits = list(found)
    if len(hits) != 1:
        consoles = ", ".join(f"{ip} ({info[1]}, host-id {info[0]})" for ip, info in found.items()) or "none"
        raise SystemExit(f"cannot pick a PS5 (found: {consoles}); set PS5_HOST or PS5_HOST_ID")
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    CACHE.write_text(hits[0] + "\n")
    return hits[0]


if __name__ == "__main__":
    if "--list" in sys.argv:
        for ip, (hid, name, ver) in discover().items():
            print(f"{ip}  {name}  host-id {hid}  system {ver}")
    else:
        print(console_ip())
