"""The local HTTP server: the web UI's files and a small JSON API.

Safety:
  - it listens on 127.0.0.1 only, on a free port picked at launch;
  - every request must name this server in its Host header (127.0.0.1:PORT
    or localhost:PORT), which stops DNS rebinding;
  - every /api/ request must carry the per-launch token in the
    X-Ghostldr-Token header.  A web page from elsewhere can't read the token
    and can't send that header here without a CORS preflight, which this
    server never allows, so it can't drive the app (CSRF);
  - POST bodies must be JSON, at most 64 KiB; requests with a foreign Origin
    or a cross-site Sec-Fetch-Site are refused;
  - the scripts run with argument lists, never through a shell.
"""

from __future__ import annotations

import hmac
import json
import mimetypes
import re
import secrets
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from . import problems
from .service import AppError, Service

WEB = Path(__file__).resolve().parents[1] / "web"
STATIC = {"/": "index.html", "/index.html": "index.html", "/app.css": "app.css", "/app.js": "app.js",
          "/icon.svg": "icon.svg"}
MAX_BODY = 64 << 10
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob: data:; "
       "connect-src 'self'; font-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


class AppServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, service: Service, port: int = 0, token: str | None = None) -> None:
        super().__init__(("127.0.0.1", port), Handler)
        self.service = service
        self.token = token or secrets.token_urlsafe(32)
        self.port = self.server_address[1]
        self.hosts = {f"127.0.0.1:{self.port}", f"localhost:{self.port}"}
        self.origins = {f"http://{h}" for h in self.hosts}
        self.quit = threading.Event()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}/"


class Handler(BaseHTTPRequestHandler):
    server: AppServer
    protocol_version = "HTTP/1.1"
    server_version = "ghostldr-app"
    sys_version = ""

    def log_message(self, fmt, *args) -> None:     # keep the terminal quiet
        pass

    # ---- responses ----

    def _headers(self, status: int, ctype: str, length: int, extra: dict | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Content-Security-Policy", CSP)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()

    def send_json(self, obj, status: int = 200) -> None:
        body = json.dumps(obj, separators=(",", ":")).encode()
        self._headers(status, "application/json; charset=utf-8", len(body))
        self.wfile.write(body)

    def send_error_json(self, status: int, message: str, **extra) -> None:
        self.send_json({"error": message, **extra}, status)

    # ---- checks ----

    def host_ok(self) -> bool:
        return self.headers.get("Host", "") in self.server.hosts

    def origin_ok(self) -> bool:
        origin = self.headers.get("Origin")
        if origin is not None and origin not in self.server.origins:
            return False
        site = self.headers.get("Sec-Fetch-Site")
        return site in (None, "same-origin", "none")

    def token_ok(self) -> bool:
        got = self.headers.get("X-Ghostldr-Token", "")
        return bool(got) and hmac.compare_digest(got.encode(), self.server.token.encode())

    def guard(self, api: bool) -> bool:
        if not self.host_ok():
            self.send_error_json(421 if api else 403, "wrong host")
            return False
        if not self.origin_ok():
            self.send_error_json(403, "cross-site request refused")
            return False
        if api and not self.token_ok():
            self.send_error_json(401, "missing or wrong token")
            return False
        return True

    def body(self) -> dict:
        ctype = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            raise AppError("request too large", 413)
        raw = self.rfile.read(length) if length else b""
        if ctype != "application/json":
            raise AppError("JSON only", 415)
        try:
            data = json.loads(raw or b"{}")
        except ValueError:
            raise AppError("bad JSON")
        if not isinstance(data, dict):
            raise AppError("bad JSON")
        return data

    # ---- dispatch ----

    def do_GET(self) -> None:
        url = urlsplit(self.path)
        if url.path.startswith("/api/"):
            self.api("GET", url.path, parse_qs(url.query))
            return
        if not self.guard(api=False):
            return
        name = STATIC.get(url.path)
        if not name:
            self.send_error_json(404, "not found")
            return
        data = (WEB / name).read_bytes()
        ctype = {"html": "text/html; charset=utf-8", "css": "text/css; charset=utf-8",
                 "js": "text/javascript; charset=utf-8", "svg": "image/svg+xml"}.get(
            name.rsplit(".", 1)[-1], mimetypes.guess_type(name)[0] or "application/octet-stream")
        self._headers(200, ctype, len(data))
        self.wfile.write(data)

    def do_POST(self) -> None:
        url = urlsplit(self.path)
        if not url.path.startswith("/api/"):
            self.send_error_json(404, "not found")
            return
        self.api("POST", url.path, parse_qs(url.query))

    def do_OPTIONS(self) -> None:                     # no CORS, ever
        self.send_error_json(405, "no")

    do_PUT = do_DELETE = do_PATCH = do_OPTIONS

    def api(self, method: str, path: str, query: dict) -> None:
        if not self.guard(api=True):
            return
        svc = self.server.service
        svc.last_request = time.time()
        try:
            data = self.body() if method == "POST" else {}
            route = ROUTES.get((method, path))
            if route:
                result = route(self, svc, data, query)
            else:
                m = re.fullmatch(r"/api/jobs/([0-9-]+)(/cancel)?", path)
                a = re.fullmatch(r"/api/art/([A-Za-z0-9_-]{1,15})", path)
                if m and method == "GET" and not m[2]:
                    job = svc.jobs.get(m[1])
                    if not job:
                        raise AppError("no such job", 404)
                    since = int(query.get("since", ["0"])[0] or 0)
                    wait = float(query.get("wait", ["0"])[0] or 0)
                    result = job.read(since, wait)
                elif m and method == "POST" and m[2]:
                    result = {"cancelled": svc.jobs.cancel(m[1])}
                elif a and method == "GET":
                    png = svc.art(a[1])
                    if png is None:
                        raise AppError("no art", 404)
                    self._headers(200, "image/png", len(png))
                    self.wfile.write(png)
                    return
                else:
                    raise AppError("not found", 404)
            if result is None:
                result = {"ok": True}
            elif hasattr(result, "summary"):
                result = {"job": result.summary()}
            self.send_json(result)
        except AppError as e:
            self.send_error_json(e.status, str(e), **e.extra)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:      # never take the server down
            self.send_error_json(500, f"internal error: {type(e).__name__}: {e}")


def _b(data: dict, key: str) -> bool:
    return data.get(key) is True


def _s(data: dict, key: str) -> str:
    v = data.get(key, "")
    return v if isinstance(v, str) else ""


def _quit(h: Handler, svc: Service, data: dict, q: dict):
    running = svc.jobs.running()
    if running and not _b(data, "force"):
        raise AppError("Something is still running.", 409, running=[j.label for j in running])
    h.server.quit.set()
    return {"ok": True}


ROUTES = {
    ("GET", "/api/ping"): lambda h, s, d, q: {"ok": True, "app": "ghostldr"},
    ("GET", "/api/state"): lambda h, s, d, q: s.state(),
    ("GET", "/api/loader"): lambda h, s, d, q: s.loader_status(),
    ("GET", "/api/jobs"): lambda h, s, d, q: {"jobs": s.jobs.list()},
    ("GET", "/api/runlog"): lambda h, s, d, q: s.runlog(int(q.get("tail", ["400"])[0] or 400)),
    ("POST", "/api/discover"): lambda h, s, d, q: s.discover(),
    ("POST", "/api/console/select"): lambda h, s, d, q: {"console": s.select_console(_s(d, "ip"),
                                                                                     _s(d, "host_id"))},
    ("POST", "/api/console/forget"): lambda h, s, d, q: s.forget_console(),
    ("GET", "/api/trouble"): lambda h, s, d, q: {"problems": problems.catalogue()},
    ("POST", "/api/doctor"): lambda h, s, d, q: s.doctor(_s(d, "title")),
    ("POST", "/api/folder/pick"): lambda h, s, d, q: s.pick_folder(),
    ("POST", "/api/folder/list"): lambda h, s, d, q: s.list_dir(_s(d, "path") or None),
    ("POST", "/api/dump/inspect"): lambda h, s, d, q: s.inspect_dump(_s(d, "path")),
    ("POST", "/api/pack"): lambda h, s, d, q: s.pack(_s(d, "src"), _s(d, "title"), _b(d, "fetch_art")),
    ("POST", "/api/report"): lambda h, s, d, q: s.report(_s(d, "src")),
    ("POST", "/api/install"): lambda h, s, d, q: s.install(_s(d, "title"), _b(d, "dry_run"),
                                                           _b(d, "no_fit_check")),
    ("POST", "/api/library/refresh"): lambda h, s, d, q: s.library_refresh(),
    ("POST", "/api/library/remove"): lambda h, s, d, q: s.library_remove(_s(d, "title"), _s(d, "confirm")),
    ("POST", "/api/library/meta"): lambda h, s, d, q: s.library_meta(_s(d, "title"), _b(d, "fetch_art")),
    ("POST", "/api/boot"): lambda h, s, d, q: s.boot(_s(d, "title"), _b(d, "select"), _b(d, "force_preflight")),
    ("POST", "/api/stubs"): lambda h, s, d, q: s.stubs(),
    ("POST", "/api/manager/start"): lambda h, s, d, q: s.manager_start(),
    ("POST", "/api/manager/stop"): lambda h, s, d, q: s.manager_stop(),
    ("POST", "/api/open/manager"): lambda h, s, d, q: s.open_url(_s(d, "url")),
    ("POST", "/api/open/folder"): lambda h, s, d, q: s.open_folder(_s(d, "which")),
    ("POST", "/api/settings"): lambda h, s, d, q: {"settings": s.settings.update(
        **{k: d[k] for k in ("dump", "title", "fetch_art") if k in d})},
    ("POST", "/api/quit"): _quit,
}
