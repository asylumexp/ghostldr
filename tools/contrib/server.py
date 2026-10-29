#!/usr/bin/env python3
"""The collection server for community memory plans (docs/community-plans.md).

Players who share their learned memory plans send them here from the
computer that starts the boot; the daily job (sync_plans.py) pulls what came
in and writes the repository's plans/ folder.  Python 3.11, standard library
only, one file, so it runs anywhere with a python3.

    POST /v1/plans                    contribution key: add or replace a plan
    GET  /v1/submissions?after=<id>   admin key: what came in after <id>
    GET  /v1/health                   no key: {"ok": true}

Keys go in `Authorization: Bearer <key>`, compared in constant time.  The
contribution key ships in release builds and only adds plans; the admin key
stays with the daily job.

One row is kept per install, eboot fingerprint and platform: a new plan from
the same install replaces its row and takes a new, higher id, so a pull with
`after` sees it again.  The same plan sent again changes nothing (200).

Each client address may POST 30 times an hour.  Behind a Cloudflare tunnel
every request comes from the tunnel, so the client's own address is taken
from CF-Connecting-IP, but only when the TCP peer is a trusted proxy
(--trusted-proxies; by default any private or loopback address).

    python3 server.py --db plans.db --contrib-key-file contrib.key --admin-key-file admin.key
    python3 server.py --listen 127.0.0.1:8790 ...

Every option can also come from the environment: GHOSTLDR_PLANS_DB,
GHOSTLDR_PLANS_LISTEN, GHOSTLDR_PLANS_CONTRIB_KEY_FILE,
GHOSTLDR_PLANS_ADMIN_KEY_FILE, GHOSTLDR_PLANS_TRUSTED_PROXIES.  One line is
logged per request, to stderr, without keys or plan bodies.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import collections
import datetime
import hmac
import ipaddress
import json
import os
import re
import sqlite3
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# struct am_plan (ghostldr/native/src/automem.c, AM_VERSION 1)
PLAN_MAGIC = 0x314D454D52444C47      # "GLDRMEM1"
PLAN_VERSION = 1
PLAN_SIZE = 1080
PLAN_FP = slice(16, 36)              # the eboot's fingerprint

MAX_BODY = 8 << 10                   # a 1080-byte plan is ~1.5 KiB of base64
RATE_LIMIT = 30                      # POSTs per client address ...
RATE_WINDOW = 3600.0                 # ... per hour
PAGE = 500                           # submissions per GET

# a title ID (CUSA17419), or a homebrew one of up to 15 such characters
TITLE = re.compile(r"[A-Z]{4}\d{5}|[A-Z0-9_-]{1,15}")
FINGERPRINT = re.compile(r"[0-9a-f]{40}")
INSTALL = re.compile(r"[0-9a-fA-F]{32}")
VERSION = re.compile(r"[\w.+-]{0,32}")
PLATFORMS = ("ps5", "ps4")


class Invalid(ValueError):
    """A submission that breaks the contract; the message says how."""


def check_plan(plan: bytes, fingerprint: str) -> None:
    """Raise Invalid unless `plan` is a version 1 am_plan for `fingerprint`."""
    if len(plan) != PLAN_SIZE:
        raise Invalid(f"plan is {len(plan)} bytes, not {PLAN_SIZE}")
    magic, version, size = struct.unpack_from("<QII", plan, 0)
    if magic != PLAN_MAGIC:
        raise Invalid("plan magic is not GLDRMEM1")
    if version != PLAN_VERSION or size != PLAN_SIZE:
        raise Invalid(f"plan version {version} size {size}, not {PLAN_VERSION} {PLAN_SIZE}")
    if plan[PLAN_FP].hex() != fingerprint:
        raise Invalid("plan is for another fingerprint")


def check_submission(sub: object) -> dict:
    """The contract's checks for one submission (a POST body, or a row the
    daily job pulled).  Returns it normalised, with "plan" as bytes."""
    if not isinstance(sub, dict):
        raise Invalid("body is not a JSON object")

    def field(name: str, pattern: re.Pattern, required: bool = True) -> str:
        value = sub.get(name, None if required else "")
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise Invalid(f"bad {name}")
        return value

    out = {
        "install": field("install", INSTALL).lower(),
        "title": field("title", TITLE),
        "fingerprint": field("fingerprint", FINGERPRINT),
        "ghostldr": field("ghostldr", VERSION, required=False),
    }
    platform = sub.get("platform")
    if platform not in PLATFORMS:
        raise Invalid("bad platform")
    out["platform"] = platform
    plan = sub.get("plan")
    if not isinstance(plan, str):
        raise Invalid("bad plan")
    try:
        out["plan"] = base64.b64decode(plan, validate=True)
    except (binascii.Error, ValueError):
        raise Invalid("plan is not base64") from None
    check_plan(out["plan"], out["fingerprint"])
    return out


class Store:
    """The SQLite database: one row per (install, fingerprint, platform).
    AUTOINCREMENT keeps ids from ever being reused, so a replaced row's new
    id is higher than any a puller has seen."""

    def __init__(self, path: str) -> None:
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("""CREATE TABLE IF NOT EXISTS submissions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            received TEXT NOT NULL,
            install TEXT NOT NULL,
            title TEXT NOT NULL,
            platform TEXT NOT NULL,
            fingerprint TEXT NOT NULL,
            plan BLOB NOT NULL,
            ghostldr TEXT NOT NULL,
            UNIQUE (install, fingerprint, platform))""")
        self.db.commit()

    def add(self, sub: dict) -> tuple[bool, int]:
        """Store `sub`; (True, new id) when stored, (False, id) when the
        install's row already holds this plan."""
        key = (sub["install"], sub["fingerprint"], sub["platform"])
        with self.lock, self.db:
            row = self.db.execute("SELECT id, title, plan FROM submissions WHERE install=? AND fingerprint=? "
                                  "AND platform=?", key).fetchone()
            if row and row[1] == sub["title"] and bytes(row[2]) == sub["plan"]:
                return False, row[0]
            if row:
                self.db.execute("DELETE FROM submissions WHERE id=?", (row[0],))
            received = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            cur = self.db.execute("INSERT INTO submissions (received, install, title, platform, fingerprint, "
                                  "plan, ghostldr) VALUES (?,?,?,?,?,?,?)",
                                  (received, sub["install"], sub["title"], sub["platform"], sub["fingerprint"],
                                   sub["plan"], sub["ghostldr"]))
            return True, cur.lastrowid

    def after(self, after: int, limit: int = PAGE) -> list[dict]:
        """Rows with an id above `after`, oldest first, plans in base64."""
        with self.lock:
            rows = self.db.execute("SELECT id, received, install, title, platform, fingerprint, plan, ghostldr "
                                   "FROM submissions WHERE id>? ORDER BY id LIMIT ?", (after, limit)).fetchall()
        names = ("id", "received", "install", "title", "platform", "fingerprint", "plan", "ghostldr")
        out = [dict(zip(names, r)) for r in rows]
        for r in out:
            r["plan"] = base64.b64encode(r["plan"]).decode()
        return out


class RateLimit:
    """At most `limit` events per address in any `window` seconds."""

    def __init__(self, limit: int = RATE_LIMIT, window: float = RATE_WINDOW) -> None:
        self.limit, self.window = limit, window
        self.lock = threading.Lock()
        self.seen: dict[str, collections.deque] = {}
        self.swept = time.monotonic()

    def allow(self, addr: str) -> bool:
        now = time.monotonic()
        with self.lock:
            if now - self.swept > self.window:      # forget addresses gone quiet
                self.seen = {a: q for a, q in self.seen.items() if q and now - q[-1] < self.window}
                self.swept = now
            q = self.seen.setdefault(addr, collections.deque())
            while q and now - q[0] >= self.window:
                q.popleft()
            if len(q) >= self.limit:
                return False
            q.append(now)
            return True


def parse_proxies(text: str) -> list | None:
    """--trusted-proxies: "private" (None: any private or loopback address),
    "" (trust none) or a comma-separated list of addresses and networks."""
    text = text.strip()
    if text == "private":
        return None
    return [ipaddress.ip_network(p.strip(), strict=False) for p in text.split(",") if p.strip()]


class Handler(BaseHTTPRequestHandler):
    server: "PlanServer"
    server_version = "ghostldr-plans"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    # -- helpers --------------------------------------------------------------

    def client(self) -> str:
        """The client's address: CF-Connecting-IP when the peer is a trusted
        proxy and the header holds an address, else the peer itself."""
        peer = self.client_address[0]
        headers = getattr(self, "headers", None)       # missing on a malformed request line
        header = headers.get("CF-Connecting-IP", "").strip() if headers else ""
        if header and self.server.trusts(peer):
            try:
                return str(ipaddress.ip_address(header))
            except ValueError:
                pass
        return peer

    def key_is(self, key: bytes) -> bool:
        auth = self.headers.get("Authorization", "")
        scheme, _, given = auth.partition(" ")
        return scheme.lower() == "bearer" and hmac.compare_digest(given.strip().encode(), key)

    def reply(self, status: int, body: dict) -> None:
        data = json.dumps(body, separators=(",", ":")).encode() + b"\n"
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def fail(self, status: int, error: str) -> None:
        self.note = error
        self.reply(status, {"ok": False, "error": error})

    def log_request(self, code="-", size="-") -> None:
        # one line per request, the client's real address, never a key or a plan
        url = urllib.parse.urlsplit(getattr(self, "path", "") or "-")
        after = urllib.parse.parse_qs(url.query).get("after", [""])[0]
        where = url.path[:80] + (f"?after={after}" if after.isdigit() else "")
        note = getattr(self, "note", "")
        sys.stderr.write(f"{self.log_date_time_string()} {self.client()} {self.command} {where} {code}"
                         f"{' ' + note if note else ''}\n")

    def log_message(self, format: str, *args) -> None:
        pass                    # http.server's own lines (errors) would carry the raw request line

    # -- requests -------------------------------------------------------------

    def do_GET(self) -> None:
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/v1/health":
            return self.reply(200, {"ok": True})
        if url.path == "/v1/submissions":
            if not self.key_is(self.server.admin_key):
                return self.fail(401, "bad key")
            after = urllib.parse.parse_qs(url.query).get("after", ["0"])[0]
            if not after.isdigit():
                return self.fail(400, "bad after")
            rows = self.server.store.after(int(after))
            return self.reply(200, {"submissions": rows, "last": rows[-1]["id"] if rows else int(after)})
        self.fail(404, "not found")

    def do_POST(self) -> None:
        if urllib.parse.urlsplit(self.path).path != "/v1/plans":
            return self.fail(404, "not found")
        self.close_connection = True       # bodies we refuse are never read
        if not self.server.limit.allow(self.client()):
            return self.fail(429, "too many")
        if not self.key_is(self.server.contrib_key):
            return self.fail(401, "bad key")
        length = self.headers.get("Content-Length", "")
        if not length.isdigit():
            return self.fail(411 if not length else 400, "need Content-Length")
        if int(length) > MAX_BODY:
            return self.fail(413, "too big")
        try:
            sub = check_submission(json.loads(self.rfile.read(int(length))))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return self.fail(400, "body is not JSON")
        except Invalid as e:
            return self.fail(400, str(e))
        stored, row = self.server.store.add(sub)
        self.note = f"{sub['title']} {sub['platform']} #{row}{'' if stored else ' unchanged'}"
        self.reply(201 if stored else 200, {"ok": True, "id": row, "stored": stored})

    def do_PUT(self) -> None:
        self.fail(405, "method not allowed")

    do_DELETE = do_PATCH = do_PUT


class PlanServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, listen: tuple[str, int], store: Store, contrib_key: bytes, admin_key: bytes,
                 proxies: list | None = None, limit: RateLimit | None = None) -> None:
        if not contrib_key or not admin_key or contrib_key == admin_key:
            raise ValueError("the contribution and admin keys must be set and differ")
        self.store, self.contrib_key, self.admin_key = store, contrib_key, admin_key
        self.proxies, self.limit = proxies, limit or RateLimit()
        super().__init__(listen, Handler)

    def trusts(self, peer: str) -> bool:
        try:
            addr = ipaddress.ip_address(peer)
        except ValueError:
            return False
        if self.proxies is None:
            return addr.is_private or addr.is_loopback
        return any(addr in net for net in self.proxies)


def read_key(path: str) -> bytes:
    key = Path(path).read_text().strip().encode()
    if not key:
        raise SystemExit(f"{path} is empty")
    return key


def main(argv: list[str] | None = None) -> int:
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", default=env("GHOSTLDR_PLANS_DB", "plans.db"))
    ap.add_argument("--listen", default=env("GHOSTLDR_PLANS_LISTEN", "0.0.0.0:8790"), help="address:port")
    ap.add_argument("--contrib-key-file", default=env("GHOSTLDR_PLANS_CONTRIB_KEY_FILE", "contrib.key"))
    ap.add_argument("--admin-key-file", default=env("GHOSTLDR_PLANS_ADMIN_KEY_FILE", "admin.key"))
    ap.add_argument("--trusted-proxies", default=env("GHOSTLDR_PLANS_TRUSTED_PROXIES", "private"),
                    help='whose CF-Connecting-IP to believe: "private" (default), "" for none, or a list of '
                         "addresses/networks")
    args = ap.parse_args(argv)
    host, _, port = args.listen.rpartition(":")
    server = PlanServer((host.strip("[]") or "0.0.0.0", int(port)), Store(args.db),
                        read_key(args.contrib_key_file), read_key(args.admin_key_file),
                        parse_proxies(args.trusted_proxies))
    sys.stderr.write(f"ghostldr plans: listening on {args.listen}, db {args.db}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
