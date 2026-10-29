"""The console's community memory plans offline (native/plansync, docs/community-plans.md).

plansync.c (DNS, HTTP over BearSSL, GitHub's plans/index.json and plans, the
collection server) is built for this computer against
tests/selector/plansync_host.c, POSIX sockets and a folder standing in for
the GLDRPLANS container, and run against plain HTTP servers here that answer
as GitHub's API, raw.githubusercontent.com and the collection server do.

    python3 -m unittest discover -s ghostldr/tests -p test_plansync.py

GHOSTLDR_LIVE_TLS=1 also checks the TLS handshakes with GitHub's real hosts.
"""

from __future__ import annotations

import base64
import hashlib
import http.server
import json
import os
import shutil
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent / "native"
BEARSSL = NATIVE / "third_party/bearssl"
REPO_PATH = "/repos/asylumexp/ghostldr/contents/plans/index.json"
FP = "0ac6bc17db533c799dcccaa4fcf85ebf00000000"


def plan(fp: str = FP, boots: int = 3, oom: int = 0, alive: int = 7) -> bytes:
    """A struct am_plan (automem.c): magic, version, size, fingerprint, then the counters."""
    b = bytearray(1080)
    struct.pack_into("<QII", b, 0, 0x314D454D52444C47, 1, 1080)
    b[16:36] = bytes.fromhex(fp)
    struct.pack_into("<9I", b, 36, boots, 3908, 3908, 3908, 3907, oom, 0, 0, alive)
    return bytes(b)


class Fake(http.server.BaseHTTPRequestHandler):
    """GitHub's API and raw hosts, and the collection server, as one server:
    what each answers is in the server's `state`."""

    def log_message(self, *args) -> None:
        pass

    def reply(self, status: int, body: bytes = b"", headers: dict | None = None, chunked: bool = False) -> None:
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 333):     # small chunks: a chunk's CRLF lands anywhere in a read
                piece = body[i:i + 333]
                self.wfile.write(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_GET(self) -> None:
        st = self.server.state
        st["requests"].append(("GET", self.path, dict(self.headers)))
        if self.path.startswith(REPO_PATH):
            if st.get("rate_limited"):
                return self.reply(403, b'{"message": "API rate limit exceeded"}', {"X-RateLimit-Remaining": "0"})
            if self.headers.get("If-None-Match") == st["etag"]:
                return self.reply(304, headers={"ETag": st["etag"], "X-RateLimit-Remaining": "59"})
            return self.reply(200, st["index"], {"ETag": st["etag"], "X-RateLimit-Remaining": "58"}, chunked=True)
        name = self.path.rsplit("/", 1)[-1]
        if name in st["plans"]:
            return self.reply(200, st["plans"][name])
        self.reply(404, b"not found")

    def do_POST(self) -> None:
        st = self.server.state
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        st["requests"].append(("POST", self.path, dict(self.headers)))
        if self.headers.get("Authorization") != "Bearer " + st["key"]:
            return self.reply(401, b'{"error": "bad key"}')
        st["posted"].append(json.loads(body))
        self.reply(st.get("post_status", 201), b'{"ok": true}')


class PlanSyncTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "plansync_host"
        cc = shutil.which("cc") or shutil.which("clang")
        if not cc or not BEARSSL.is_dir():
            raise unittest.SkipTest("no C compiler or no BearSSL tree")
        srcs = [str(p) for p in sorted((BEARSSL / "src").rglob("*.c"))]
        subprocess.run([cc, "-O1", "-w", f"-I{BEARSSL / 'inc'}", f"-I{BEARSSL / 'src'}", f"-I{NATIVE / 'plansync'}",
                        "-DBR_RDRAND=0", "-DBR_USE_URANDOM=0", "-DBR_USE_UNIX_TIME=0", "-DBR_AES_X86NI=0",
                        "-DBR_SSE2=0", "-DBR_POWER8=0", "-o", str(cls.exe),
                        str(NATIVE / "plansync/plansync.c"), str(NATIVE / "plansync/plansync_ta.c"),
                        str(HERE / "selector/plansync_host.c"), *srcs], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def setUp(self) -> None:
        self.store = Path(tempfile.mkdtemp(dir=self.tmp.name))
        good = plan()
        other = plan("11" * 20)
        self.plans = {f"{FP}-ps5.bin": good, f"{'11' * 20}-ps4.bin": other, f"{'22' * 20}-ps5.bin": plan("22" * 20)}
        index = {"version": 1, "plans": [
            {"title": "CUSA17419", "fingerprint": FP, "platform": "ps5", "sha256": hashlib.sha256(good).hexdigest(),
             "contributors": 4, "boots": 3, "updated": "2026-10-29"},
            {"title": "CUSA11456", "fingerprint": "11" * 20, "platform": "ps4",
             "sha256": hashlib.sha256(other).hexdigest(), "contributors": 2},
            {"title": "CUSA00900", "fingerprint": "22" * 20, "platform": "ps5", "sha256": "00" * 32,   # wrong sum
             "contributors": 9},
            {"title": "CUSA99999", "fingerprint": "33" * 20, "platform": "ps5", "sha256": "00" * 32},  # not here
        ]}
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        self.server.state = {"index": json.dumps(index, indent=1).encode(), "etag": '"v1"', "plans": self.plans,
                             "requests": [], "posted": [], "key": "k" * 64}
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addr = f"127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def run_job(self, *args: str) -> list[str]:
        p = subprocess.run([str(self.exe), str(self.store), "127.0.0.1", "--github", self.addr, self.addr, *args],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout.splitlines()

    def test_fetch_keeps_the_index_and_the_plans_for_the_games_here(self) -> None:
        out = self.run_job("CUSA17419", "CUSA11456", "CUSA00900")
        self.assertIn("state done github 200 shared 0 unshared 0 plans 2", out)
        self.assertIn(f"plan CUSA17419 ps5 {FP} contributors 4", out)
        self.assertIn(f"plan CUSA11456 ps4 {'11' * 20} contributors 2", out)
        self.assertFalse([ln for ln in out if "CUSA00900" in ln and ln.startswith("plan ")])  # its sum is wrong
        self.assertEqual((self.store / f"plan-{FP}-ps5.bin").read_bytes(), self.plans[f"{FP}-ps5.bin"])
        self.assertEqual((self.store / "plans-etag.txt").read_text(), '"v1"')
        self.assertFalse((self.store / f"plan-{'22' * 20}-ps5.bin").exists())
        api = [r for r in self.server.state["requests"] if r[1].startswith(REPO_PATH)]
        self.assertEqual(api[0][2].get("Accept"), "application/vnd.github.raw+json")

    def test_a_304_uses_what_was_kept(self) -> None:
        self.run_job("CUSA17419")
        self.server.state["requests"].clear()
        out = self.run_job("CUSA17419")
        self.assertIn("state done github 304 shared 0 unshared 0 plans 1", out)
        sent = self.server.state["requests"]
        self.assertEqual(sent[0][2].get("If-None-Match"), '"v1"')
        self.assertEqual(len(sent), 1)                      # the plan came from the store, not the network

    def test_the_rate_limit_offers_nothing_and_says_nothing_on_screen(self) -> None:
        self.run_job("CUSA17419")                          # a kept copy of the index and the plan
        self.server.state["rate_limited"] = True
        out = self.run_job("CUSA17419")
        self.assertIn("state failed github 403 shared 0 unshared 0 plans 0", out)

    def test_no_server_offers_nothing(self) -> None:
        p = subprocess.run([str(self.exe), str(self.store), "127.0.0.1", "--github", "127.0.0.1:9", "127.0.0.1:9",
                            "CUSA17419"], capture_output=True, text=True, timeout=60)
        self.assertIn("state failed github 0 shared 0 unshared 0 plans 0", p.stdout)

    def outbox(self, title: str = "CUSA17419", fp: str = FP, data: bytes | None = None) -> Path:
        f = self.store / f"share-{title}-{fp}-ps5.bin"
        f.write_bytes(plan(fp) if data is None else data)
        return f

    def test_share_sends_the_outbox_and_empties_it(self) -> None:
        f = self.outbox()
        out = self.run_job("--share", self.addr, "k" * 64, "--no-fetch", "X")
        self.assertIn("state failed github 0 shared 1 unshared 0 plans 0", out)
        self.assertFalse(f.exists())
        sent = self.server.state["posted"][0]
        self.assertEqual((sent["title"], sent["platform"], sent["fingerprint"]), ("CUSA17419", "ps5", FP))
        self.assertEqual(base64.b64decode(sent["plan"]), plan())
        self.assertRegex(sent["install"], r"^[0-9a-f]{32}$")
        self.assertEqual((self.store / "install-id.txt").read_text(), sent["install"])

    def test_share_keeps_what_the_server_did_not_take(self) -> None:
        f = self.outbox()
        self.server.state["post_status"] = 503
        out = self.run_job("--share", self.addr, "k" * 64, "--no-fetch", "X")
        self.assertIn("state failed github 0 shared 0 unshared 1 plans 0", out)
        self.assertTrue(f.exists())
        f2 = self.outbox("CUSA11456", "11" * 20)
        out = self.run_job("--share", self.addr, "wrong", "--no-fetch", "X")   # 401: refused for good
        self.assertFalse(f.exists() or f2.exists(), out)

    def test_share_drops_a_file_that_isnt_a_plan(self) -> None:
        f = self.outbox(data=b"\0" * 1080)
        self.run_job("--share", self.addr, "k" * 64, "--no-fetch", "X")
        self.assertFalse(f.exists())
        self.assertEqual(self.server.state["posted"], [])

    def test_a_build_without_an_address_keeps_the_outbox(self) -> None:
        f = self.outbox()
        p = subprocess.run([str(self.exe), str(self.store), "127.0.0.1", "--github", self.addr, self.addr,
                            "--share", "", "", "--no-fetch", "X"], capture_output=True, text=True, timeout=60)
        self.assertIn("unshared 1", p.stdout)
        self.assertTrue(f.exists())

    @unittest.skipUnless(os.environ.get("GHOSTLDR_LIVE_TLS"), "GHOSTLDR_LIVE_TLS=1 checks GitHub's real hosts")
    def test_live_tls_with_github(self) -> None:
        p = subprocess.run([str(self.exe), str(self.store), "1.1.1.1", "CUSA17419"], capture_output=True,
                           text=True, timeout=60)
        self.assertRegex(p.stdout, r"github (200|304|403|404) ")  # an answer over TLS (403: the rate limit)


if __name__ == "__main__":
    unittest.main()
