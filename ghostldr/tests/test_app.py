#!/usr/bin/env python3
"""The desktop app's backend (app/ghostapp): the local server's token and
Host checks, background jobs against fake scripts (start, stream, cancel,
one console job at a time), the parsers that read the host scripts' output,
and the settings file.

Where it can, a parser test feeds the line the script itself would print
(console.py's Console.describe, gfs_catalog's Entry.describe, run.py's
doctor format, exec_fit's summary), so a change of format shows up here.

    cd ghostldr/tests && python3 -m unittest test_app
"""

from __future__ import annotations

import http.client
import json
import os
import sys
import tempfile
import textwrap
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app"))
sys.path.insert(0, str(ROOT / "ghostldr/host"))
sys.path.insert(0, str(ROOT / "ghostldr/tools"))

from ghostapp import jobs as J  # noqa: E402
from ghostapp import parsers as P  # noqa: E402
from ghostapp import problems  # noqa: E402
from ghostapp import settings as ST  # noqa: E402
from ghostapp import trackers as T  # noqa: E402
from ghostapp import workspace as W  # noqa: E402
from ghostapp.server import AppServer  # noqa: E402
from ghostapp.service import AppError, Service  # noqa: E402

import console as host_console  # noqa: E402  (ghostldr/host/console.py)
import exec_fit  # noqa: E402
import gfs_catalog as gc  # noqa: E402


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text), encoding="utf-8")
    return path


def wait_for(cond, timeout: float = 10.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


class TempDirs(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.root = self.tmp / "release"
        self.cfg = self.tmp / "config"
        (self.root / "ghostldr/host").mkdir(parents=True)
        (self.root / "ghostldr/tools").mkdir(parents=True)

    def tearDown(self) -> None:
        self._tmp.cleanup()


# ---------------------------------------------------------------- server

class ServerTest(TempDirs):
    def setUp(self) -> None:
        super().setUp()
        self.service = Service(self.root, cfg=self.cfg)
        self.server = AppServer(self.service, 0)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        self.port = self.server.port
        self.host = f"127.0.0.1:{self.port}"

    def tearDown(self) -> None:
        self.service.jobs.cancel_all(wait=5)
        self.server.shutdown()
        self.server.server_close()
        super().tearDown()

    def request(self, method: str, path: str, body: bytes | None = None, headers: dict | None = None,
                token: str | None = "auto", host: str | None = None) -> tuple[int, dict]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        h = {"Host": host or self.host}
        if token == "auto":
            token = self.server.token
        if token is not None:
            h["X-Ghostldr-Token"] = token
        h.update(headers or {})
        conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
        for k, v in h.items():
            conn.putheader(k, v)
        if body is not None:
            conn.putheader("Content-Length", str(len(body)))
        conn.endheaders(body)
        r = conn.getresponse()
        raw = r.read()
        conn.close()
        try:
            data = json.loads(raw)
        except ValueError:
            data = {"raw": raw}
        return r.status, data

    def post(self, path: str, obj: dict, **kw) -> tuple[int, dict]:
        headers = {"Content-Type": "application/json", **kw.pop("headers", {})}
        return self.request("POST", path, json.dumps(obj).encode(), headers, **kw)

    def test_listens_on_loopback_only(self) -> None:
        self.assertEqual(self.server.server_address[0], "127.0.0.1")
        self.assertNotEqual(self.port, 0)

    def test_token_is_random_per_launch(self) -> None:
        other = AppServer(self.service, 0)
        try:
            self.assertNotEqual(other.token, self.server.token)
            self.assertGreaterEqual(len(self.server.token), 32)
        finally:
            other.server_close()

    def test_api_needs_the_token(self) -> None:
        self.assertEqual(self.request("GET", "/api/state", token=None)[0], 401)
        self.assertEqual(self.request("GET", "/api/state", token="wrong")[0], 401)
        self.assertEqual(self.request("GET", "/api/state", token=self.server.token[:-1])[0], 401)
        status, data = self.request("GET", "/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(data["version"], "0.5.0")

    def test_every_api_route_checks_the_token(self) -> None:
        from ghostapp.server import ROUTES
        for method, path in ROUTES:
            body = b"{}" if method == "POST" else None
            headers = {"Content-Type": "application/json"} if body else {}
            status, _ = self.request(method, path, body, headers, token=None)
            self.assertEqual(status, 401, f"{method} {path}")
        self.assertEqual(self.request("GET", "/api/jobs/1-1", token=None)[0], 401)
        self.assertEqual(self.request("GET", "/api/art/CUSA11456", token=None)[0], 401)

    def test_host_header_is_checked(self) -> None:
        # DNS rebinding: a page on another name that resolves here
        self.assertEqual(self.request("GET", "/api/state", host=f"evil.example:{self.port}")[0], 421)
        self.assertEqual(self.request("GET", "/", host=f"evil.example:{self.port}")[0], 403)
        self.assertEqual(self.request("GET", "/", host="127.0.0.1")[0], 403)        # no port
        self.assertEqual(self.request("GET", "/", host=f"127.0.0.1:{self.port + 1}")[0], 403)
        self.assertEqual(self.request("GET", "/", host=f"localhost:{self.port}", token=None)[0], 200)
        self.assertEqual(self.request("GET", "/api/state", host=f"localhost:{self.port}")[0], 200)

    def test_cross_site_requests_are_refused(self) -> None:
        self.assertEqual(self.request("GET", "/api/state", headers={"Origin": "http://evil.example"})[0], 403)
        self.assertEqual(self.request("GET", "/api/state", headers={"Sec-Fetch-Site": "cross-site"})[0], 403)
        self.assertEqual(self.request("GET", "/api/state", headers={"Origin": f"http://{self.host}"})[0], 200)
        self.assertEqual(self.request("GET", "/api/state", headers={"Sec-Fetch-Site": "same-origin"})[0], 200)

    def test_posts_must_be_json(self) -> None:
        # a form or a no-cors fetch from another site can only send these simple types
        for ctype in ("text/plain", "application/x-www-form-urlencoded", "multipart/form-data"):
            status, _ = self.request("POST", "/api/console/forget", b"{}", {"Content-Type": ctype})
            self.assertEqual(status, 415, ctype)
        self.assertEqual(self.post("/api/console/forget", {})[0], 200)
        self.assertEqual(self.request("POST", "/api/console/forget", b"x" * (65 << 10),
                                      {"Content-Type": "application/json"})[0], 413)

    def test_no_cors_preflight(self) -> None:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("OPTIONS", "/api/state", headers={"Host": self.host, "Origin": "http://evil.example",
                                                       "Access-Control-Request-Method": "POST",
                                                       "Access-Control-Request-Headers": "x-ghostldr-token"})
        r = conn.getresponse()
        r.read()
        conn.close()
        self.assertEqual(r.status, 405)
        self.assertIsNone(r.getheader("Access-Control-Allow-Origin"))
        self.assertIsNone(r.getheader("Access-Control-Allow-Headers"))

    def test_static_files_and_headers(self) -> None:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request("GET", "/", headers={"Host": self.host})
        r = conn.getresponse()
        page = r.read().decode()
        self.assertEqual(r.status, 200)
        self.assertIn("ghostldr", page)
        self.assertIn("default-src 'none'", r.getheader("Content-Security-Policy"))
        self.assertEqual(r.getheader("X-Frame-Options"), "DENY")
        conn.request("GET", "/../../etc/passwd", headers={"Host": self.host})
        r = conn.getresponse()
        r.read()
        self.assertEqual(r.status, 404)
        conn.close()
        for name in ("/app.js", "/app.css", "/icon.svg"):
            self.assertEqual(self.request("GET", name, token=None)[0], 200, name)

    def test_select_console_validates_the_address(self) -> None:
        with mock.patch.object(Service, "console_module") as mod:
            mod.return_value.probe.return_value = None
            self.assertEqual(self.post("/api/console/select", {"ip": "not an ip"})[0], 400)
            self.assertEqual(self.post("/api/console/select", {"ip": "127.0.0.1"})[0], 400)
            status, data = self.post("/api/console/select", {"ip": "192.0.2.44"})
        self.assertEqual(status, 200)
        self.assertEqual(data["console"]["ip"], "192.0.2.44")
        self.assertEqual(self.service.settings.get("console")["ip"], "192.0.2.44")

    def test_console_jobs_need_a_console(self) -> None:
        status, data = self.post("/api/doctor", {})
        self.assertEqual(status, 409)
        self.assertEqual(data.get("action"), "goto_console")

    def test_quit_refuses_while_running_unless_forced(self) -> None:
        script = write(self.tmp / "sleepy.py", "import time\nprint('up', flush=True)\ntime.sleep(30)\n")
        job = self.service.jobs.start("test", "Sleeping", J.python_argv(script), {"console"})
        self.assertTrue(wait_for(lambda: job.next_seq >= 1))
        status, data = self.post("/api/quit", {})
        self.assertEqual(status, 409)
        self.assertFalse(self.server.quit.is_set())
        self.assertEqual(self.post("/api/quit", {"force": True})[0], 200)
        self.assertTrue(self.server.quit.is_set())


# ---------------------------------------------------------------- jobs

class JobTest(TempDirs):
    def setUp(self) -> None:
        super().setUp()
        self.jobs = J.JobManager(self.tmp, self.cfg / "logs")

    def tearDown(self) -> None:
        self.jobs.cancel_all(wait=5)
        super().tearDown()

    def script(self, body: str) -> list[str]:
        return J.python_argv(write(self.tmp / f"fake{time.monotonic_ns()}.py", body))

    def test_start_stream_finish(self) -> None:
        argv = self.script("""
            import sys, time
            for i in range(5):
                print(f"line {i}", flush=True)
                time.sleep(0.05)
            sys.stdout.write("cr 1\\rcr 2\\r\\nlast")
        """)
        job = self.jobs.start("test", "Counting", argv, set(), T.Tracker())
        first = job.read(0, wait=5)
        self.assertTrue(first["output"])
        self.assertEqual(first["output"][0], "line 0")
        self.assertTrue(wait_for(lambda: not job.running))
        out = job.read(0)
        self.assertEqual(out["output"], [f"line {i}" for i in range(5)] + ["cr 1", "cr 2", "last"])
        self.assertEqual((job.state, job.rc), ("done", 0))
        self.assertEqual(out["next"], 8)
        self.assertEqual(job.read(8)["output"], [])           # nothing new after the cursor
        log = next((self.cfg / "logs").glob("*-test.log")).read_text()
        self.assertIn("line 4", log)
        self.assertIn("# exit 0 (done)", log)

    def test_read_waits_for_new_lines(self) -> None:
        argv = self.script("import time\ntime.sleep(0.4)\nprint('late', flush=True)\ntime.sleep(0.2)\n")
        job = self.jobs.start("test", "Late", argv)
        t0 = time.time()
        out = job.read(0, wait=5)
        self.assertEqual(out["output"], ["late"])
        self.assertLess(time.time() - t0, 4)

    def test_cancel_stops_the_script(self) -> None:
        argv = self.script("""
            import time
            print("started", flush=True)
            try:
                while True:
                    time.sleep(0.05)
            except KeyboardInterrupt:
                print("interrupted", flush=True)
                raise SystemExit(0)
        """)
        job = self.jobs.start("test", "Forever", argv, {"console"})
        self.assertTrue(wait_for(lambda: job.next_seq >= 1))
        self.assertTrue(self.jobs.cancel(job.id))
        self.assertTrue(wait_for(lambda: not job.running, 10))
        self.assertEqual(job.state, "cancelled")
        self.assertIn("interrupted", job.lines)                # it got Ctrl+C, not a kill
        self.assertFalse(self.jobs.cancel(job.id))             # nothing left to cancel

    def test_cancel_kills_a_script_that_ignores_ctrl_c(self) -> None:
        argv = self.script("""
            import signal, time
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            print("stubborn", flush=True)
            while True:
                time.sleep(0.05)
        """)
        job = self.jobs.start("test", "Stubborn", argv)
        self.assertTrue(wait_for(lambda: job.next_seq >= 1))
        with mock.patch.object(J, "GRACE", 0.3):
            t = threading.Thread(target=J.stop_process, args=(job.proc, 0.3))
            t.start()
            t.join(10)
        self.assertTrue(wait_for(lambda: not job.running, 10))
        self.assertNotEqual(job.rc, 0)

    def test_one_console_job_at_a_time(self) -> None:
        argv = self.script("import time\nprint('x', flush=True)\ntime.sleep(30)\n")
        first = self.jobs.start("install", "Installing", argv, {"console", "pack"})
        with self.assertRaises(J.Busy) as ctx:
            self.jobs.start("boot", "Playing", argv, {"console", "ports"})
        self.assertIs(ctx.exception.job, first)
        other = self.jobs.start("report", "Report", self.script("print('ok')\n"), set())   # no shared lock
        self.assertTrue(wait_for(lambda: not other.running))
        self.jobs.cancel(first.id)
        self.assertTrue(wait_for(lambda: not first.running, 10))
        again = self.jobs.start("boot", "Playing", self.script("print('ok')\n"), {"console"})
        self.assertTrue(wait_for(lambda: not again.running))
        self.assertEqual(again.state, "done")

    def test_failure_is_diagnosed(self) -> None:
        argv = self.script("raise SystemExit(\"receiver did not come up (is SWRR at the Luac0re loader?)\")\n")
        job = self.jobs.start("install", "Installing", argv)
        self.assertTrue(wait_for(lambda: not job.running))
        self.assertEqual(job.state, "failed")
        self.assertEqual(job.problem["id"], "loader")
        self.assertEqual(job.problem["action"], "wait_loader")
        self.assertIn("receiver did not come up", job.error)

    def test_a_script_that_cannot_start(self) -> None:
        job = self.jobs.start("test", "Missing", [str(self.tmp / "no-such-python"), "x.py"])
        self.assertEqual(job.state, "failed")
        self.assertIn("could not start", job.error)

    def test_never_through_a_shell(self) -> None:
        calls = []
        real = J.subprocess.Popen

        def spy(argv, **kw):
            calls.append((argv, kw))
            return real(argv, **kw)
        with mock.patch.object(J.subprocess, "Popen", spy):
            job = self.jobs.start("test", "Quoting", self.script("import sys\nprint(sys.argv[1])\n") + ["a; rm -rf ~ $(x)"])
        self.assertTrue(wait_for(lambda: not job.running))
        argv, kw = calls[0]
        self.assertIsInstance(argv, list)
        self.assertNotIn("shell", kw)
        self.assertEqual(job.lines, ["a; rm -rf ~ $(x)"])

    def test_lines_kept_in_memory_are_capped(self) -> None:
        argv = self.script("for i in range(130):\n    print(i)\n")
        with mock.patch.object(J, "MAX_LINES", 100):
            job = self.jobs.start("test", "Many", argv)
            self.assertTrue(wait_for(lambda: not job.running))
        out = job.read(0)
        self.assertTrue(out["truncated"])
        self.assertEqual(out["since"], 30)
        self.assertEqual(out["output"][0], "30")


class ServiceJobTest(TempDirs):
    """Service runs the real script names: fake ones stand in for them."""

    def setUp(self) -> None:
        super().setUp()
        self.service = Service(self.root, cfg=self.cfg)

    def tearDown(self) -> None:
        self.service.jobs.cancel_all(wait=5)
        super().tearDown()

    def wait(self, job):
        self.assertTrue(wait_for(lambda: not job.running))
        return job

    def test_discovery_remembers_the_console_with_the_loader(self) -> None:
        a = host_console.Console("192.0.2.10", "AAAA11112222", "Living room", "PS5", "13600007", True, loader=9026)
        b = host_console.Console("192.0.2.11", "BBBB33334444", "Bedroom", "PS5", "14200000", False)
        write(self.root / "ghostldr/host/console.py", f"""
            print("1) " + {a.describe()!r})
            print("2) " + {b.describe()!r})
        """)
        job = self.wait(self.service.discover())
        self.assertEqual(job.state, "done")
        self.assertEqual([c["ip"] for c in self.service.consoles], ["192.0.2.10", "192.0.2.11"])
        chosen = self.service.settings.get("console")
        self.assertEqual((chosen["ip"], chosen["host_id"], chosen["name"]), ("192.0.2.10", "AAAA11112222", "Living room"))
        # the console moved (DHCP): found again by its host-id
        write(self.root / "ghostldr/host/console.py", f"""
            print("1) " + {a.describe().replace("192.0.2.10", "192.0.2.77")!r})
        """)
        self.wait(self.service.discover())
        self.assertEqual(self.service.settings.get("console")["ip"], "192.0.2.77")

    def test_scripts_get_the_console_address(self) -> None:
        self.service.settings.update(console={"ip": "192.0.2.10", "host_id": "", "name": "", "kind": "PS5"})
        write(self.root / "ghostldr/host/run.py", """
            import os, sys
            print("PASS  " + "python 3.11.0".ljust(22) + " ")
            print("args", sys.argv[1:], "host", os.environ.get("PS_HOST"))
        """)
        job = self.wait(self.service.doctor("cusa11456"))
        self.assertIn("args ['doctor', '--title', 'CUSA11456'] host 192.0.2.10", job.lines)
        self.assertEqual(job.tracker.snapshot()["counts"]["PASS"], 1)

    def test_no_console_job_while_the_save_manager_runs(self) -> None:
        self.service.settings.update(console={"ip": "192.0.2.10"})
        self.service.manager = {"url": "http://192.0.2.10:8090/", "token": "", "running": True}
        with self.assertRaises(AppError) as ctx:
            self.service.library_refresh()
        self.assertEqual(ctx.exception.status, 409)

    def test_install_needs_a_pack(self) -> None:
        self.service.settings.update(console={"ip": "192.0.2.10"})
        with self.assertRaises(AppError):
            self.service.install("CUSA11456")
        with self.assertRaises(AppError):
            self.service.install("not a title!")

    def test_boot_arguments(self) -> None:
        self.service.settings.update(console={"ip": "192.0.2.10"})
        for n in ("core.bin", "core.json", "ext.bin", "selector.bin"):
            write(self.root / "build/ghostldr" / n, "x")
        write(self.root / "ghostldr/host/run.py", "import sys\nprint(sys.argv[1:])\n")
        job = self.wait(self.service.boot("CUSA11456"))
        self.assertEqual(job.lines, ["['boot', '--app-root', 'ghostfs', '--no-build', '--title', 'CUSA11456']"])
        job = self.wait(self.service.boot("", select=True, force_preflight=True))
        self.assertEqual(job.lines, ["['boot', '--app-root', 'ghostfs', '--no-build', '--select', '--force-preflight']"])

    def test_install_progress_and_library(self) -> None:
        self.service.settings.update(console={"ip": "192.0.2.10"})
        write(self.root / "build/ghostfs/CUSA00900/layout.json", json.dumps(
            {"title": "CUSA00900", "name": "A Game", "version": "01.09", "total": 3 << 30, "prefix": "G00900",
             "containers": [{}, {}, {}], "src": ""}))
        other = gc.Entry("CUSA11456", "GHOST", "GHOSTINDEX", total=int(53.3 * 2**30), containers=56)
        write(self.root / "ghostldr/host/gfs_install.py", f"""
            import time
            def log(m): print(time.strftime('%H:%M:%S') + ' gfs-install: ' + m, flush=True)
            log("executable memory: fits the EE buffer (code 21.9 MiB)")
            log("receiver sent to 192.0.2.10:9026")
            log("connected to the receiver on 9027")
            log("installed: " + {other.describe()!r})
            log("installing CUSA00900 in 3 containers G00900000.. (3.0 GiB)")
            log("  send 3 containers, 3.0 GiB")
            log("  space: 3.0 GiB needed, at least 40.0 GiB free")
            for i in range(3):
                log(f"[{{i + 1}}/3] G0090000{{i}}: 992 MiB in 70 s (console 68 s); avg 14.0 MB/s, ETA {{2 - i}} min")
            log("install complete")
        """)
        job = self.wait(self.service.install("cusa00900"))
        self.assertEqual(job.state, "done")
        pr = job.tracker.snapshot()
        self.assertEqual((pr["done"], pr["send_containers"], pr["sent_mib"], pr["phase"]), (3, 3, 2976, "done"))
        self.assertEqual(pr["fit"]["kind"], "fits")
        self.assertEqual(pr["space"], {"need_gib": 3.0, "free_gib": 40.0, "at_least": True})
        lib = {e["title"]: e for e in self.service.library["entries"]}
        self.assertTrue(lib["CUSA00900"]["complete"])
        self.assertEqual(lib["CUSA00900"]["name"], "A Game")
        self.assertIn("CUSA11456", lib)
        self.assertEqual(self.service.settings.get("title"), "CUSA00900")


# ---------------------------------------------------------------- parsers

class InstallParserTest(unittest.TestCase):
    def test_progress_line(self) -> None:
        ev = P.parse_install_line("12:00:01 gfs-install: [12/31] G00900011: 992 MiB in 76 s (console 74 s); "
                                  "avg 13.8 MB/s, ETA 25 min")
        self.assertEqual(ev, {"event": "progress", "index": 12, "count": 31, "container": "G00900011", "mib": 992,
                              "seconds": 76, "rate_mbs": 13.8, "eta_min": 25})
        self.assertEqual(P.parse_install_line("[1/56] GHOST000: 992 MiB in 75 s (console 73 s); avg 13.2 MB/s, "
                                              "ETA 70 min")["count"], 56)

    def test_other_lines(self) -> None:
        cases = {
            "  send 31 containers, 30.0 GiB": {"event": "plan", "containers": 31, "gib": 30.0},
            "  space: 30.0 GiB needed, 12.5 GiB free": {"event": "space", "need_gib": 30.0, "free_gib": 12.5,
                                                         "at_least": False},
            "  nothing to do: already installed": {"event": "nothing"},
            "install complete": {"event": "complete"},
            "dry run: nothing changed": {"event": "dry_run_done"},
            "receiver sent to 192.0.2.10:9026": {"event": "receiver"},
            "connected to the receiver on 9027": {"event": "connected"},
            "removed": {"event": "removed"},
            "nothing installed": {"event": "installed", "entry": None},
            "  update GFSCATALOG": {"event": "step", "text": "update GFSCATALOG"},
            "  write the index (the data is already there)": {"event": "step",
                                                              "text": "write the index (the data is already there)"},
        }
        for line, want in cases.items():
            self.assertEqual(P.parse_install_line("09:10:11 gfs-install: " + line), want, line)
        ev = P.parse_install_line("installing CUSA00900 in 31 containers G00900000.. (29.3 GiB, incomplete)")
        self.assertEqual((ev["title"], ev["containers"], ev["prefix"], ev["gib"], ev["resume"]),
                         ("CUSA00900", 31, "G00900", 29.3, True))
        self.assertIsNone(P.parse_install_line("something else entirely"))

    def test_catalogue_entries_as_the_installer_prints_them(self) -> None:
        e = gc.Entry("CUSA00900", "G00900", "G00900INDEX", name="Bloodborne™", version="01.09",
                     total=int(29.3 * 2**30), containers=31)
        got = P.parse_entry(e.describe())
        self.assertEqual(got, {"title": "CUSA00900", "name": "Bloodborne™", "version": "01.09", "containers": 31,
                               "prefix": "G00900", "gib": 29.3, "index": "G00900INDEX", "complete": True})
        e = gc.Entry("LAPY20001", "LAPY", "LAPYINDEX", name="Some Homebrew App", total=2**27, containers=1,
                     flags=gc.INCOMPLETE)
        got = P.parse_entry(e.describe())
        self.assertEqual((got["name"], got["version"], got["complete"]), ("Some Homebrew App", "", False))
        got = P.parse_entry(gc.Entry("CUSA11456", "GHOST", "GHOSTINDEX", total=2**30, containers=56).describe())
        self.assertEqual((got["title"], got["name"]), ("CUSA11456", ""))

    def test_library_listing(self) -> None:
        entries = [gc.Entry("CUSA00900", "G00900", "G00900INDEX", name="Bloodborne™", version="01.09",
                            total=31 << 30, containers=31),
                   gc.Entry("CUSA11456", "GHOST", "GHOSTINDEX", total=53 << 30, containers=56)]
        text = "\n".join(["12:00:00 gfs-install: receiver sent to 192.0.2.10:9026",
                          "12:00:01 gfs-install: 2 titles installed"] +
                         [f"12:00:01 gfs-install:   {e.describe()}" for e in entries])
        lib = P.parse_library(text)
        self.assertTrue(lib["ok"])
        self.assertEqual([e["title"] for e in lib["entries"]], ["CUSA00900", "CUSA11456"])
        empty = P.parse_library("12:00:01 gfs-install: 0 titles installed (no GFSCATALOG yet)")
        self.assertEqual((empty["ok"], empty["count"], empty["catalogue"]), (True, 0, False))
        self.assertFalse(P.parse_library("receiver did not come up")["ok"])

    def test_tracker_follows_an_install(self) -> None:
        tr = T.InstallTracker()
        for line in ["executable memory: fits the EE buffer (code 21.9 MiB)",
                     "receiver sent to 192.0.2.10:9026", "connected to the receiver on 9027", "nothing installed",
                     "installing CUSA00900 in 31 containers G00900000.. (29.3 GiB)",
                     "  send 31 containers, 29.3 GiB",
                     "[1/31] G00900000: 992 MiB in 76 s (console 74 s); avg 13.8 MB/s, ETA 34 min",
                     "[2/31] G00900001: 992 MiB in 70 s (console 68 s); avg 14.0 MB/s, ETA 33 min"]:
            tr.feed("10:00:00 gfs-install: " + line)
        s = tr.snapshot()
        self.assertEqual((s["phase"], s["done"], s["send_containers"], s["sent_mib"], s["eta_min"], s["rate_mbs"]),
                         ("sending", 2, 31, 1984, 33, 14.0))
        self.assertTrue(s["surveyed"])
        self.assertEqual(s["installed"], [])


class FitParserTest(unittest.TestCase):
    def test_verdicts(self) -> None:
        self.assertEqual(P.parse_fit("executable memory: fits the EE buffer (code 21.9 MiB)"),
                         {"kind": "fits", "text": "fits the EE buffer (code 21.9 MiB)", "reclaim": False})
        self.assertEqual(P.parse_fit("12:00:00 gfs-install: executable memory: needs multi-buffer placement "
                                     "(eboot.bin: ee 32 + vu0 8 MiB buffers over its code, 3.0 MiB in direct "
                                     "memory)")["kind"], "multi")
        got = P.parse_fit("executable memory: won't fit with buffer views; would fit after reclaim (eboot.bin: "
                          "60.0 MiB of code, 70.0 of 80.00 MiB of JIT memory with the other modules)")
        self.assertEqual((got["kind"], got["reclaim"]), ("nofit", True))
        self.assertEqual(P.parse_fit("executable memory: not checked (OSError: boom)")["kind"], "unknown")
        self.assertIsNone(P.parse_fit("selector: CUSA00900: name ..."))

    def test_matches_exec_fit_summaries(self) -> None:
        fits = {"verdict": exec_fit.FITS, "modules": [{"text": 21 << 20}]}
        self.assertEqual(P.parse_fit("executable memory: " + exec_fit.summary(fits))["kind"], "fits")
        nofit = {"verdict": exec_fit.NO_FIT, "modules": [], "runs": [(0, 200 << 20)], "big": "eboot.bin"}
        self.assertEqual(P.parse_fit("executable memory: " + exec_fit.summary(nofit))["kind"], "nofit")

    def test_pack_output(self) -> None:
        text = "\n".join([
            "selector: CUSA00900: name 'Bloodborne™' (param.sfo), icon: the title's own, backdrop: the title's own",
            "CUSA00900 (own profile): 9,000 files (120 from the open order first), 300 dirs, 29.3 GiB in 31 "
            "containers G00900000..; layout 1a2b3c4d".replace("9,000", "9000"),
            "  G00900000: 992 MiB, 12 pieces, starts with eboot.bin",
            "executable memory: fits the EE buffer (code 21.9 MiB)",
        ])
        r = P.parse_pack(text)
        self.assertEqual((r["title"], r["files"], r["gib"], r["containers"], r["prefix"], r["own_profile"]),
                         ("CUSA00900", 9000, 29.3, 31, "G00900", True))
        self.assertEqual(r["fit"]["kind"], "fits")
        self.assertTrue(r["selector"].startswith("CUSA00900: name"))


class ConsoleParserTest(unittest.TestCase):
    def line(self, n: int, c) -> str:
        return f"{n}) {c.describe()}"

    def test_console_list_as_console_py_prints_it(self) -> None:
        on = host_console.Console("192.0.2.10", "AAAA11112222", "Living room PS5", "PS5", "13600007", True,
                                  loader=9026)
        rest = host_console.Console("192.0.2.11", "BBBB33334444", "Spare", "PS5", "14200000", False)
        ps4 = host_console.Console("192.0.2.12", "CCCC", "Old  one", "PS4", "12020001", True, "CUSA03474",
                                   "Star Wars Racer Revenge")
        bare = host_console.Console("192.0.2.13")
        text = "\n".join(self.line(i + 1, c) for i, c in enumerate([on, rest, ps4, bare]))
        got = P.parse_console_list(text)
        self.assertEqual(len(got), 4)
        self.assertEqual(got[0], {"ip": "192.0.2.10", "kind": "PS5", "name": "Living room PS5",
                                  "host_id": "AAAA11112222", "firmware": "13.60", "on": True, "running": "",
                                  "swrr": "", "loader": True})
        self.assertEqual((got[1]["on"], got[1]["loader"], got[1]["firmware"]), (False, False, "14.20"))
        self.assertEqual((got[2]["kind"], got[2]["name"], got[2]["swrr"]), ("PS4", "Old  one", "US"))
        self.assertTrue(got[2]["running"].startswith("CUSA03474"))
        self.assertEqual((got[3]["kind"], got[3]["name"], got[3]["host_id"], got[3]["firmware"]), ("", "", "", ""))
        self.assertEqual(P.parse_console_list("no PS4 or PS5 answered (UDP 987 / 9302)"), [])


class DoctorParserTest(unittest.TestCase):
    @staticmethod
    def row(status: str, label: str, detail: str = "") -> str:
        return f"{status:<4}  {label:<22} {detail}"           # run.py doctor's own format

    def test_rows(self) -> None:
        text = "\n".join([
            self.row("PASS", "python 3.11.9"),
            self.row("PASS", "core.bin integrity", "185344 bytes, core.json bin_size 185344"),
            self.row("PASS", "lua payload", "379531 of 512000 bytes"),
            self.row("PASS", "UDP 18194 (log)", "free"),
            self.row("FAIL", "TCP 18195 (extension)", "cannot bind: [Errno 48] Address already in use"),
            self.row("FAIL", "console loader", "nothing at 192.0.2.10:9026 (timed out); SWRR up with Luac0re's loader?"),
            self.row("WARN", "title dump", "staged, but no name table (build/nid/known_names.txt) for a coverage check"),
        ])
        rows = P.parse_doctor(text)
        self.assertEqual([r["key"] for r in rows], ["python", "core", "payload", "log_port", "ext_port", "loader", "dump"])
        self.assertEqual([r["status"] for r in rows], ["PASS"] * 4 + ["FAIL", "FAIL", "WARN"])
        self.assertEqual(rows[4]["label"], "TCP 18195 (extension)")
        self.assertTrue(rows[5]["detail"].startswith("nothing at 192.0.2.10:9026"))
        self.assertNotIn("functions", rows[6])

    def test_title_dump_numbers(self) -> None:
        r = P.parse_doctor_line(self.row("WARN", "title dump", "CUSA00900: 1204 functions, 37 unresolved"))
        self.assertEqual((r["title"], r["functions"], r["unresolved"]), ("CUSA00900", 1204, 37))
        r = P.parse_doctor_line(self.row("PASS", "title dump", "CUSA11456: 889 functions, 0 unresolved"))
        self.assertEqual((r["status"], r["functions"], r["unresolved"]), ("PASS", 889, 0))
        tr = T.DoctorTracker()
        tr.feed(self.row("PASS", "python 3.12.1"))
        tr.feed("not a row")
        tr.feed(self.row("FAIL", "prebuilt core", "missing core.bin in build/ghostldr"))
        self.assertEqual(tr.snapshot()["counts"], {"PASS": 1, "WARN": 0, "FAIL": 1})


class BootAndOtherParserTest(unittest.TestCase):
    def test_boot_stages(self) -> None:
        tr = T.BootTracker("CUSA11456")
        lines = [
            "preflight: CUSA11456: 889 functions; 0 unresolved",
            "serving the extension (2617474 bytes) on TCP 18195",
            "serving the game selector (137256 bytes) on TCP 18196",
            "sending 379531 bytes of Lua to 192.0.2.10:9026",
            "listening for the console's log; press Ctrl+C to stop (the game keeps running)",
            "16:45:01 [ghostldr 812] ghostldr core at 0x9009d8000, ctx flags 6",
            "extension: sent 2617474 bytes",
            "16:45:02 [ghostldr 812] ghostfs: CUSA11456, 40116 entries, 54571 MiB in 56 containers of 992 MiB",
            "16:45:03 [ghostldr 812] main thread: starting eboot.bin",
            "16:45:04 [ghostldr 812] ext: no extension from the computer",
            "  [hint] the core could not fetch ext.bin: allow TCP 18195 in this computer's firewall",
        ]
        for line in lines:
            tr.feed(line)
        s = tr.snapshot()
        self.assertEqual(s["stage"], "starting")
        self.assertTrue(s["listening"])
        self.assertEqual(s["preflight"]["state"], "info")
        self.assertEqual(s["hints"], ["the core could not fetch ext.bin: allow TCP 18195 in this computer's firewall"])
        tr.feed("16:45:05 [ghostldr 812] ghostldr core at 0x1")      # stages never go back
        self.assertEqual(tr.snapshot()["stage"], "starting")
        self.assertEqual(P.parse_boot_line("preflight: no terminal to ask on; re-run with --force-preflight")
                         ["state"], "stopped")
        self.assertEqual(P.parse_boot_line("preflight: the selector picks the title on the console; skipped")
                         ["state"], "skipped")
        self.assertEqual(P.parse_boot_line("16:45:09 [ghostldr 812] CRASH SIGSEGV at eboot.bin+1234")["event"], "crash")

    def test_manager_output(self) -> None:
        text = "\n  ghostldr manager is running\n  open   http://192.0.2.10:8090/\n  token  K7QF-3MXP   (also shown " \
               "on the PS5)\n\n12:00:00 manager: detached: stop it from the page (Exit) or with --exit"
        self.assertEqual(P.parse_manager(text), {"url": "http://192.0.2.10:8090/", "token": "K7QF-3MXP",
                                                 "running": True, "stopped": False})
        stop = P.parse_manager("12:00:00 manager: stopping the manager on 192.0.2.10:8090: done")
        self.assertEqual((stop["running"], stop["stopped"]), (False, True))

    def test_detect(self) -> None:
        d = P.parse_detect("title CUSA00900 (param.sfo TITLE_ID CUSA00900, APP_VER 01.09), eboot fingerprint "
                           + "ab" * 20)
        self.assertEqual((d["title"], d["version"], d["homebrew"]), ("CUSA00900", "01.09", False))
        d = P.parse_detect("title ? (param.sfo TITLE_ID -, APP_VER -), eboot fingerprint OpenOrbis's generic one, "
                           "module hello")
        self.assertEqual((d["title"], d["sfo_title"], d["homebrew"], d["module"]), ("", "", True, "hello"))

    def test_param_sfo_name(self) -> None:
        from ghostapp.service import sfo_strings
        from test_profiles import make_sfo
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / "param.sfo"
            f.write_bytes(make_sfo({"TITLE": "Some Game™", "TITLE_ID": "CUSA00001", "APP_VER": "01.02",
                                    "PARENTAL_LEVEL": 5}))
            self.assertEqual(sfo_strings(f), {"TITLE": "Some Game™", "TITLE_ID": "CUSA00001", "APP_VER": "01.02"})
            f.write_bytes(b"\0PSF garbage")
            self.assertEqual(sfo_strings(f), {})
            self.assertEqual(sfo_strings(Path(tmp) / "missing.sfo"), {})

    def test_report_summary_knows_a_release_has_no_name_table(self) -> None:
        rep = {"inputs": {"names": 37}, "summary": {"functions": 889, "functions_by_class": {"UNRESOLVED": 742},
                                                   "import_sites": 1200},
               "executable_memory": {"verdict": "fits the EE buffer"},
               "libraries": [{"library": "libkernel", "classes": {"UNRESOLVED": 140}},
                             {"library": "libSceNet", "classes": {"UNRESOLVED": 35, "TITLE": 3}},
                             {"library": "libc", "classes": {"TITLE": 9}}]}
        s = T.summarize_report(rep)
        self.assertFalse(s["name_table"])
        self.assertEqual((s["functions"], s["fit_verdict"]), (889, "fits the EE buffer"))
        self.assertEqual([x["library"] for x in s["unresolved_libraries"]], ["libkernel", "libSceNet"])
        rep["inputs"]["names"] = 42000
        self.assertTrue(T.summarize_report(rep)["name_table"])
        self.assertIsNone(T.summarize_report(None))

    def test_known_problems(self) -> None:
        self.assertEqual(problems.diagnose(["SystemExit: not enough space on the console: CUSA00900 needs 3.0 GiB "
                                            "more"])["id"], "space")
        self.assertEqual(problems.diagnose(["extension: the console did not fetch it (is TCP 18195 open in this "
                                            "computer's firewall?)"])["id"], "ext_firewall")
        self.assertEqual(problems.diagnose(["Luac0re: OOB_ARRAY_POINTER failed, restart the application to "
                                            "retry"])["id"], "luacore")
        self.assertIsNone(problems.diagnose(["all fine"]))
        ids = [p["id"] for p in problems.catalogue()]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn("black", ids)


# ---------------------------------------------------------------- settings

class SettingsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.path = Path(self._tmp.name) / "deep/dir/settings.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_defaults_and_round_trip(self) -> None:
        s = ST.Settings(self.path)
        self.assertEqual(s.snapshot(), ST.DEFAULTS)
        s.update(dump="/games/app0", console={"ip": "192.0.2.10", "name": "Test"}, unknown="ignored")
        again = ST.Settings(self.path).snapshot()
        self.assertEqual(again["dump"], "/games/app0")
        self.assertEqual(again["console"], {"ip": "192.0.2.10", "host_id": "", "name": "Test", "kind": ""})
        self.assertNotIn("unknown", again)
        s.update(console={"host_id": "AAAA"})                         # merged, not replaced
        self.assertEqual(ST.Settings(self.path).get("console")["ip"], "192.0.2.10")

    def test_bad_files_and_values(self) -> None:
        self.path.parent.mkdir(parents=True)
        self.path.write_text("{not json")
        self.assertEqual(ST.Settings(self.path).snapshot(), ST.DEFAULTS)
        self.path.write_text(json.dumps({"dump": 5, "console": "x", "fetch_art": "yes", "title": "CUSA00900"}))
        s = ST.Settings(self.path).snapshot()
        self.assertEqual((s["dump"], s["console"], s["fetch_art"], s["title"]),
                         ("", ST.DEFAULTS["console"], False, "CUSA00900"))
        s = ST.Settings(self.path)
        s.update(fetch_art="no", dump=["x"])
        self.assertEqual((s.get("fetch_art"), s.get("dump")), (False, ""))
        self.assertEqual(list(self.path.parent.glob(".settings-*")), [])          # no temporary files left

    def test_config_dir(self) -> None:
        with mock.patch.dict(os.environ, {"GHOSTLDR_APP_CONFIG": "/tmp/x"}):
            self.assertEqual(ST.config_dir(), Path("/tmp/x"))
        env = {k: v for k, v in os.environ.items() if k not in ("GHOSTLDR_APP_CONFIG", "XDG_CONFIG_HOME", "APPDATA")}
        home = Path("/home/someone")
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(Path, "home", return_value=home):
            with mock.patch.object(ST.sys, "platform", "darwin"):
                self.assertEqual(ST.config_dir(), home / "Library/Application Support/ghostldr")
            with mock.patch.object(ST.sys, "platform", "linux"):
                self.assertEqual(ST.config_dir(), home / ".config/ghostldr")
            with mock.patch.object(ST.sys, "platform", "win32"):
                self.assertEqual(ST.config_dir(), home / "AppData/Roaming/ghostldr")
        with mock.patch.dict(os.environ, {**env, "XDG_CONFIG_HOME": "/xdg"}, clear=True), \
                mock.patch.object(ST.sys, "platform", "linux"):
            self.assertEqual(ST.config_dir(), Path("/xdg/ghostldr"))

    def test_nothing_personal_in_the_app(self) -> None:
        """No addresses but documentation ones (192.0.2.x), example ones (192.168.1.23/123) and loopback."""
        import re
        allowed = {"127.0.0.1", "192.168.1.23", "192.168.1.123", "255.255.255.255", "0.0.0.0"}
        for f in list((ROOT / "app").rglob("*.py")) + list((ROOT / "app/web").glob("*")):
            text = f.read_text(encoding="utf-8")
            for ip in re.findall(r"\b\d{1,3}(?:\.\d{1,3}){3}\b", text):
                self.assertTrue(ip in allowed or ip.startswith("192.0.2."), f"{f.name}: {ip}")


# ---------------------------------------------------------------- workspace (installed copies)

class WorkspaceTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.src = self.tmp / "installed"
        self.dest = self.tmp / "data/workspace"
        write(self.src / "ghostldr/host/run.py", "print('run')\n")
        write(self.src / "ghostldr/host/old.py", "print('old')\n")
        write(self.src / "ghostldr/data/titles/CUSA00900.json", "{}")
        write(self.src / "ghostldr/host/__pycache__/run.cpython-311.pyc", "x")
        write(self.src / "LICENSE", "MIT")
        for n in ("core.bin", "core.json", "ext.bin", "selector.bin"):
            write(self.src / "build/ghostldr" / n, f"{n} 1")
        write(self.src / "app/launch.py", "")

    def tearDown(self) -> None:
        for d, _, _ in os.walk(self.tmp):
            os.chmod(d, 0o755)
        self._tmp.cleanup()

    def readonly(self) -> None:
        for d, dirs, files in os.walk(self.src):
            for f in files:
                os.chmod(os.path.join(d, f), 0o444)
            os.chmod(d, 0o555)

    def test_copies_the_tools_and_keeps_build(self) -> None:
        if os.name != "nt":
            self.readonly()
        self.assertTrue(W.prepare(self.src, self.dest))
        self.assertEqual((self.dest / "ghostldr/host/run.py").read_text(), "print('run')\n")
        self.assertEqual((self.dest / "build/ghostldr/ext.bin").read_text(), "ext.bin 1")
        self.assertTrue((self.dest / "LICENSE").is_file())
        self.assertFalse((self.dest / "ghostldr/host/__pycache__").exists())
        self.assertFalse((self.dest / "app").exists())                  # the app itself runs from where it is
        self.assertTrue(os.access(self.dest / "ghostldr/host/run.py", os.W_OK))
        # the user's own files in build/
        write(self.dest / "build/ghostfs/CUSA00900/layout.json", "{}")
        write(self.dest / "build/ghostldr/run.log", "log")
        self.assertFalse(W.prepare(self.src, self.dest))                # unchanged: nothing copied
        # a new version: changed, added and removed files
        for d, _, _ in os.walk(self.src):
            os.chmod(d, 0o755)
        (self.src / "ghostldr/host/old.py").unlink()
        write(self.src / "ghostldr/host/new.py", "")
        (self.src / "build/ghostldr/ext.bin").chmod(0o644)
        write(self.src / "build/ghostldr/ext.bin", "ext.bin 2")
        self.assertTrue(W.prepare(self.src, self.dest))
        self.assertFalse((self.dest / "ghostldr/host/old.py").exists())
        self.assertTrue((self.dest / "ghostldr/host/new.py").exists())
        self.assertEqual((self.dest / "build/ghostldr/ext.bin").read_text(), "ext.bin 2")
        self.assertEqual((self.dest / "build/ghostfs/CUSA00900/layout.json").read_text(), "{}")
        self.assertEqual((self.dest / "build/ghostldr/run.log").read_text(), "log")
        self.assertEqual([p.name for p in self.dest.iterdir() if p.name.startswith((".new-", ".old-"))], [])

    def test_a_copy_cut_short_is_redone(self) -> None:
        W.prepare(self.src, self.dest)
        (self.dest / W.STAMP).unlink()
        (self.dest / ".new-x").mkdir()
        self.assertTrue(W.prepare(self.src, self.dest))
        self.assertFalse((self.dest / ".new-x").exists())

    def test_default_dir(self) -> None:
        with mock.patch.dict(os.environ, {"GHOSTLDR_APP_WORKSPACE": "/tmp/ws"}):
            self.assertEqual(W.default_dir(), Path("/tmp/ws"))
        env = {k: v for k, v in os.environ.items()
               if k not in ("GHOSTLDR_APP_WORKSPACE", "XDG_DATA_HOME", "LOCALAPPDATA")}
        home = Path("/home/someone")
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(Path, "home", return_value=home):
            with mock.patch.object(W.sys, "platform", "darwin"):
                self.assertEqual(W.default_dir(), home / "Library/Application Support/ghostldr/workspace")
            with mock.patch.object(W.sys, "platform", "linux"):
                self.assertEqual(W.default_dir(), home / ".local/share/ghostldr/workspace")
            with mock.patch.object(W.sys, "platform", "win32"):
                self.assertEqual(W.default_dir(), home / "AppData/Local/ghostldr/workspace")
        with mock.patch.dict(os.environ, {**env, "XDG_DATA_HOME": "/xdg"}, clear=True), \
                mock.patch.object(W.sys, "platform", "linux"):
            self.assertEqual(W.default_dir(), Path("/xdg/ghostldr/workspace"))

    @unittest.skipIf(os.name == "nt" or getattr(os, "geteuid", lambda: 1)() == 0, "POSIX permissions, not root")
    def test_launch_picks_the_tools_folder(self) -> None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("ghostldr_launch", ROOT / "app/launch.py")
        launch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(launch)
        release = self.src
        with mock.patch.object(launch, "ROOT", release), \
                mock.patch.dict(os.environ, {"GHOSTLDR_APP_WORKSPACE": str(self.dest)}):
            self.assertEqual(launch.tools_root(None), release)                    # an unpacked zip
            self.assertEqual(launch.tools_root(""), self.dest)                    # --workspace
            self.assertEqual(launch.tools_root(str(self.tmp / "x")), (self.tmp / "x").resolve())
            self.readonly()
            self.assertEqual(launch.tools_root(None), self.dest)                  # read-only: a workspace


if __name__ == "__main__":
    unittest.main()
