"""Find the PlayStation (PS4 or PS5) that runs SWRR on the LAN.

DHCP can move the console, so the host tools look it up with the PlayStation
device discovery protocol: a PS5 answers on UDP 9302 (protocol 00030010), a
PS4 on UDP 987 (protocol 00020020).  A PS4's answer names the running app;
a PS5's doesn't, so SWRR is also recognised by Luac0re's loader listening on
TCP 9026 (an empty connection runs nothing).
  PS_HOST     use this address, no lookup (PS5_HOST works too)
  PS_HOST_ID  the console's host-id (as `python3 ghostldr/host/console.py --list`
              shows it): find that console wherever it is (PS5_HOST_ID works too)
  neither     use the only console that is on; with several, the only one
              running SWRR or Luac0re's loader (anything else is an error)
The last address found is cached in build/ghostldr/console_ip.
"""

from __future__ import annotations

import os
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from swrr import REGIONS

CACHE = Path(__file__).resolve().parents[2] / "build/ghostldr/console_ip"
# (port, query) per console generation
PROTOCOLS = {
    "PS5": (9302, b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"),
    "PS4": (987, b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00020020\n"),
}
LOADER_PORT = 9026
SWRR_TITLES = {r["title"]: name.upper() for name, r in REGIONS.items()}


@dataclass
class Console:
    ip: str
    host_id: str = ""
    name: str = ""
    kind: str = ""            # "PS4" or "PS5"
    version: str = ""
    on: bool = True           # False: rest mode (status 620)
    app_id: str = ""          # running app's title ID (PS4 answers only)
    app_name: str = ""
    loader: bool = False      # Luac0re's loader accepts connections on 9026

    @property
    def swrr(self) -> str:
        """SWRR's region ("US"/"EU") if the console reports it running, else ""."""
        return SWRR_TITLES.get(self.app_id, "")

    def describe(self) -> str:
        parts = [self.ip, self.kind or "?", self.name or "?", f"host-id {self.host_id or '?'}",
                 f"system {self.version or '?'}", "on" if self.on else "rest mode"]
        if self.app_id:
            parts.append(f"running {self.app_id}" + (f" ({self.app_name})" if self.app_name else ""))
        if self.swrr:
            parts.append(f"SWRR {self.swrr}")
        if self.loader:
            parts.append(f"Luac0re loader on {LOADER_PORT}")
        return "  ".join(parts)


def parse_reply(ip: str, reply: bytes, kind: str = "") -> Console:
    """A discovery answer: a status line, then `name:value` fields."""
    lines = reply.decode(errors="replace").splitlines()
    fields = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            fields[k.strip().lower()] = v.strip()
    status = lines[0].split() if lines else []
    return Console(
        ip=ip,
        host_id=fields.get("host-id", "").upper(),
        name=fields.get("host-name", ""),
        kind=fields.get("host-type", kind).upper(),
        version=fields.get("system-version", ""),
        on=len(status) < 2 or status[1] != "620",
        app_id=fields.get("running-app-titleid", "").upper(),
        app_name=fields.get("running-app-name", ""),
    )


def loader_listening(ip: str, timeout: float = 0.6) -> bool:
    try:
        socket.create_connection((ip, LOADER_PORT), timeout=timeout).close()
        return True
    except OSError:
        return False


def discover(wait: float = 2.0, check_loader: bool = True) -> dict[str, Console]:
    """{ip: Console} of the PS4s and PS5s that answer a broadcast."""
    found: dict[str, Console] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.3)
        for port, query in PROTOCOLS.values():
            s.sendto(query, ("255.255.255.255", port))
        kinds = {port: kind for kind, (port, _) in PROTOCOLS.items()}
        deadline = time.time() + wait
        while time.time() < deadline:
            try:
                data, (ip, port) = s.recvfrom(2048)
            except socket.timeout:
                continue
            if ip not in found:
                found[ip] = parse_reply(ip, data, kinds.get(port, ""))
    if check_loader:
        for c in found.values():
            c.loader = c.on and loader_listening(c.ip)
    return found


def probe(ip: str, timeout: float = 1.5) -> Console | None:
    """Ask one address directly (PS5 first, then PS4)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout / len(PROTOCOLS))
        for kind, (port, query) in PROTOCOLS.items():
            try:
                s.sendto(query, (ip, port))
                data, _ = s.recvfrom(2048)
                return parse_reply(ip, data, kind)
            except OSError:
                continue
    return None


def host_id(ip: str, timeout: float = 1.5) -> str:
    """The console's host-id at `ip` ("" if it does not answer discovery)."""
    c = probe(ip, timeout)
    return c.host_id if c else ""


def pick(found: dict[str, Console], want_id: str = "") -> list[Console]:
    """The candidates: by host-id if given; else the consoles that are on,
    narrowed to those running SWRR or Luac0re's loader when that leaves one."""
    if want_id:
        return [c for c in found.values() if c.host_id == want_id]
    on = [c for c in found.values() if c.on]
    if len(on) > 1:
        swrr = [c for c in on if c.swrr or c.loader]
        if swrr:
            return swrr
    return on


def _env(*names: str) -> str:
    for n in names:
        if os.environ.get(n):
            return os.environ[n]
    return ""


def console_ip() -> str:
    fixed = _env("PS_HOST", "PS5_HOST")
    if fixed:
        return fixed
    want = _env("PS_HOST_ID", "PS5_HOST_ID").upper()
    cached = CACHE.read_text().strip() if CACHE.exists() else ""
    if cached and want and host_id(cached) == want:
        return cached
    found = discover(check_loader=not want)
    hits = pick(found, want)
    if len(hits) != 1:
        consoles = "; ".join(c.describe() for c in found.values()) or "none"
        raise SystemExit(f"cannot pick a console (found: {consoles}); set PS_HOST or PS_HOST_ID")
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    CACHE.write_text(hits[0].ip + "\n")
    return hits[0].ip


if __name__ == "__main__":
    if "--list" in sys.argv:
        found = discover()
        for c in found.values():
            print(c.describe())
        if not found:
            print("no PS4 or PS5 answered (UDP 987 / 9302)")
    else:
        print(console_ip())
