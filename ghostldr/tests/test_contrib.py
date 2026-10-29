"""Community memory plans' collection server and daily job
(tools/contrib/server.py, tools/contrib/sync_plans.py, docs/community-plans.md),
offline.

The server runs on 127.0.0.1, port 0, in a thread, with temporary keys and
database, and is driven over HTTP:

  - a good plan stored (201), the same plan again unchanged (200), a new
    plan from the same install replacing the row under a new, higher id;
  - a missing or wrong key (401), the admin key not accepted for adding;
  - malformed submissions (400): a plan of the wrong size, magic or version,
    one for another fingerprint, bad base64, bad title, platform, install or
    fingerprint, a body that is not JSON;
  - a body over 8 KiB (413); the 31st POST in an hour (429), counted by
    CF-Connecting-IP from a trusted proxy and by the peer otherwise;
  - /v1/submissions paged with `after` under the admin key, refused with the
    contribution key; /v1/health.

sync_plans.py then pulls from it into a temporary repository: the plan that
did not run out of memory and stayed up beats one with more boots, then
boots, then the latest; contributors counted per install; plans/index.json
sorted with the contract's fields; files left alone when nothing changed;
--dry-run writing nothing.

    python3 -m unittest discover -s ghostldr/tests -p test_contrib.py
"""

from __future__ import annotations

import base64
import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools" / "contrib"))

import server as srv          # noqa: E402
import sync_plans as sync     # noqa: E402

CONTRIB_KEY = "c0" * 32
ADMIN_KEY = "ad" * 32
FP_A = "0ac6bc17db533c799dcccaa4fcf85ebf00000000"
FP_B = "1111111111111111111111111111111111111111"


def make_plan(fingerprint: str = FP_A, boots: int = 1, oom: int = 0, alive_min: int = 5,
              magic: int = srv.PLAN_MAGIC, version: int = 1, size: int = 1080) -> bytes:
    """A struct am_plan by the contract's layout (sites and shrinks zero)."""
    head = struct.pack("<QII20s", magic, version, size, bytes.fromhex(fingerprint))
    body = struct.pack("<12I", boots, 4096, 4096, 3000, 3200, oom, 0, 512, alive_min, 0, 0, 0)
    plan = head + body
    return plan + bytes(1080 - len(plan))


def submission(install: str = "a" * 32, title: str = "CUSA17419", platform: str = "ps5",
               fingerprint: str = FP_A, plan: bytes | None = None, **plan_args) -> dict:
    plan = make_plan(fingerprint, **plan_args) if plan is None else plan
    return {"install": install, "title": title, "platform": platform, "fingerprint": fingerprint,
            "plan": base64.b64encode(plan).decode(), "ghostldr": "1.0.0"}


class ServerCase(unittest.TestCase):
    proxies: list | None = None

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.server = srv.PlanServer(("127.0.0.1", 0), srv.Store(str(Path(self.tmp.name) / "plans.db")),
                                     CONTRIB_KEY.encode(), ADMIN_KEY.encode(), self.proxies)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        thread = threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True)
        thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.log = io.StringIO()
        quiet = contextlib.redirect_stderr(self.log)       # the server's request lines
        quiet.__enter__()
        self.addCleanup(quiet.__exit__, None, None, None)

    def request(self, method: str, path: str, body: object = None, key: str | None = CONTRIB_KEY,
                headers: dict | None = None) -> tuple[int, dict]:
        if body is not None and not isinstance(body, bytes):
            body = json.dumps(body).encode()
        req = urllib.request.Request(self.url + path, data=body, method=method, headers=dict(headers or {}))
        if key:
            req.add_header("Authorization", f"Bearer {key}")
        if body is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def post(self, sub: object, **kw) -> tuple[int, dict]:
        return self.request("POST", "/v1/plans", sub, **kw)

    def submissions(self, after: int = 0, key: str = ADMIN_KEY) -> tuple[int, dict]:
        return self.request("GET", f"/v1/submissions?after={after}", key=key)


class TestServer(ServerCase):
    def test_health(self) -> None:
        self.assertEqual(self.request("GET", "/v1/health", key=None), (200, {"ok": True}))
        self.assertEqual(self.request("GET", "/nope")[0], 404)

    def test_store_repeat_replace(self) -> None:
        status, body = self.post(submission())
        self.assertEqual(status, 201)
        first = body["id"]
        status, body = self.post(submission())
        self.assertEqual((status, body["id"], body["stored"]), (200, first, False))
        # a new plan from the same install replaces its row under a higher id
        status, body = self.post(submission(boots=2))
        self.assertEqual(status, 201)
        self.assertGreater(body["id"], first)
        status, page = self.submissions()
        self.assertEqual(status, 200)
        self.assertEqual([s["id"] for s in page["submissions"]], [body["id"]])
        row = page["submissions"][0]
        self.assertEqual(base64.b64decode(row["plan"]), make_plan(boots=2))
        self.assertEqual((row["install"], row["title"], row["platform"], row["fingerprint"], row["ghostldr"]),
                         ("a" * 32, "CUSA17419", "ps5", FP_A, "1.0.0"))
        self.assertRegex(row["received"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        # other installs and platforms are rows of their own
        self.assertEqual(self.post(submission(install="b" * 32))[0], 201)
        self.assertEqual(self.post(submission(platform="ps4"))[0], 201)
        self.assertEqual(len(self.submissions()[1]["submissions"]), 3)
        # the log names neither key nor plan
        log = self.log.getvalue()
        self.assertNotIn(CONTRIB_KEY, log)
        self.assertNotIn(ADMIN_KEY, log)
        self.assertNotIn(submission()["plan"][:40], log)
        self.assertIn("POST /v1/plans 201", log)

    def test_keys(self) -> None:
        self.assertEqual(self.post(submission(), key=None)[0], 401)
        self.assertEqual(self.post(submission(), key="0" * 64)[0], 401)
        self.assertEqual(self.post(submission(), key=ADMIN_KEY)[0], 401)
        self.assertEqual(self.post(submission(), key=CONTRIB_KEY[:-1])[0], 401)
        self.assertEqual(self.submissions(key=CONTRIB_KEY)[0], 401)
        self.assertEqual(self.request("GET", "/v1/submissions", key=None)[0], 401)
        self.assertEqual(self.submissions()[1], {"submissions": [], "last": 0})

    def test_malformed(self) -> None:
        bad = {
            "short plan": submission(plan=make_plan()[:1079]),
            "long plan": submission(plan=make_plan() + b"\0"),
            "magic": submission(magic=0x324D454D52444C47),
            "version": submission(version=2),
            "size field": submission(size=1084),
            "fingerprint mismatch": submission(plan=make_plan(FP_B)),
            "base64": dict(submission(), plan="not base64!"),
            "title": submission(title="cusa17419"),
            "long homebrew title": submission(title="A" * 16),
            "platform": submission(platform="ps3"),
            "install": submission(install="a" * 31),
            "upper fingerprint": dict(submission(), fingerprint=FP_A.upper()),
            "missing plan": {k: v for k, v in submission().items() if k != "plan"},
            "array": [submission()],
        }
        for name, sub in bad.items():
            with self.subTest(name):
                self.assertEqual(self.post(sub)[0], 400)
        self.assertEqual(self.post(b"{not json")[0], 400)
        self.assertEqual(self.submissions()[1]["submissions"], [])
        # homebrew title IDs and either case of install are accepted
        self.assertEqual(self.post(submission(title="LAPY10010"))[0], 201)
        self.assertEqual(self.post(submission(title="MY_GAME-2", install="AB" * 16))[0], 201)
        self.assertEqual(self.submissions()[1]["submissions"][-1]["install"], "ab" * 16)

    def test_too_big(self) -> None:
        body = json.dumps(dict(submission(), pad="x" * (8 << 10))).encode()
        self.assertEqual(self.post(body)[0], 413)

    def test_rate_limit_by_peer(self) -> None:
        for i in range(30):
            self.assertIn(self.post(submission(boots=i + 1))[0], (200, 201))
        self.assertEqual(self.post(submission())[0], 429)
        # behind a trusted proxy (127.0.0.1 is private), each client counts apart
        self.assertEqual(self.post(submission(), headers={"CF-Connecting-IP": "203.0.113.7"})[0], 201)

    def test_paging(self) -> None:
        ids = [self.post(submission(install=f"{i:032x}"))[1]["id"] for i in range(5)]
        self.assertEqual(self.submissions(ids[2])[1]["last"], ids[-1])
        self.assertEqual([s["id"] for s in self.submissions(ids[2])[1]["submissions"]], ids[3:])
        self.assertEqual(self.submissions(ids[-1])[1], {"submissions": [], "last": ids[-1]})
        self.assertEqual(self.request("GET", "/v1/submissions?after=x", key=ADMIN_KEY)[0], 400)


class TestUntrustedProxy(ServerCase):
    proxies = srv.parse_proxies("10.9.9.9/32")

    def test_header_ignored(self) -> None:
        cf = {"CF-Connecting-IP": "203.0.113.7"}
        for i in range(30):
            self.assertIn(self.post(submission(boots=i + 1), headers=cf)[0], (200, 201))
        # 127.0.0.1 isn't a trusted proxy, so all of them were counted against it
        self.assertEqual(self.post(submission(), headers={"CF-Connecting-IP": "198.51.100.1"})[0], 429)
        self.assertIn("127.0.0.1 POST", self.log.getvalue())

    def test_parse(self) -> None:
        self.assertIsNone(srv.parse_proxies("private"))
        self.assertEqual(srv.parse_proxies(""), [])
        self.assertEqual(len(srv.parse_proxies("192.168.1.10, 10.0.0.0/8")), 2)


class TestSync(ServerCase):
    def setUp(self) -> None:
        super().setUp()
        self.repo = Path(self.tmp.name) / "repo"
        self.state = Path(self.tmp.name) / "state.json"

    def run_sync(self, dry_run: bool = False) -> str:
        with contextlib.redirect_stdout(io.StringIO()):          # --dry-run's "would write" lines
            return sync.sync(self.url, ADMIN_KEY, self.repo, self.state, dry_run)

    def index(self) -> dict:
        return json.loads((self.repo / "plans" / "index.json").read_text())

    def test_nothing(self) -> None:
        self.assertEqual(self.run_sync(), "no new submissions")
        self.assertFalse((self.repo / "plans").exists())

    def test_selection_and_index(self) -> None:
        # FP_A ps5: many boots but ran out of memory / short-lived, vs a healthy run
        self.post(submission(install="1" * 32, boots=9, oom=1))
        self.post(submission(install="2" * 32, boots=8, alive_min=1))
        self.post(submission(install="3" * 32, boots=3))
        # FP_B ps4 (a homebrew title): equal health, most boots wins
        self.post(submission(install="1" * 32, title="LAPY10010", platform="ps4", fingerprint=FP_B, boots=4))
        self.post(submission(install="2" * 32, title="LAPY10010", platform="ps4", fingerprint=FP_B, boots=6))

        dry = self.run_sync(dry_run=True)
        self.assertTrue(dry.startswith("dry run: 2 new plans"), dry)
        self.assertFalse(self.repo.exists())
        self.assertFalse(self.state.exists())

        summary = self.run_sync()
        self.assertEqual(summary, "2 new plans, 0 updated: CUSA17419 ps5, LAPY10010 ps4")
        a = self.repo / "plans" / "CUSA17419" / f"{FP_A}-ps5.bin"
        b = self.repo / "plans" / "LAPY10010" / f"{FP_B}-ps4.bin"
        self.assertEqual(a.read_bytes(), make_plan(boots=3))
        self.assertEqual(b.read_bytes(), make_plan(FP_B, boots=6))
        index = self.index()
        self.assertEqual(index["version"], 1)
        self.assertEqual([(e["title"], e["platform"]) for e in index["plans"]],
                         [("CUSA17419", "ps5"), ("LAPY10010", "ps4")])
        e = index["plans"][0]
        self.assertEqual(list(e), ["title", "fingerprint", "platform", "path", "sha256", "contributors", "boots",
                                   "updated"])
        self.assertEqual((e["fingerprint"], e["path"], e["contributors"], e["boots"]),
                         (FP_A, f"plans/CUSA17419/{FP_A}-ps5.bin", 3, 3))
        self.assertEqual(e["sha256"], hashlib.sha256(make_plan(boots=3)).hexdigest())
        self.assertRegex(e["updated"], r"^\d{4}-\d\d-\d\d$")
        self.assertEqual(index["plans"][1]["contributors"], 2)

        # nothing new: nothing written
        stamp = a.stat().st_mtime_ns, (self.repo / "plans" / "index.json").stat().st_mtime_ns
        self.assertEqual(self.run_sync(), "no new submissions")
        self.assertEqual((a.stat().st_mtime_ns, (self.repo / "plans" / "index.json").stat().st_mtime_ns), stamp)

        # the healthy install's next plan wins on boots alone among healthy ones; ties go to the latest
        self.post(submission(install="4" * 32, boots=3, alive_min=10))
        summary = self.run_sync()
        self.assertEqual(summary, "0 new plans, 1 updated: CUSA17419 ps5")
        self.assertEqual(a.read_bytes(), make_plan(boots=3, alive_min=10))
        self.assertEqual(self.index()["plans"][0]["contributors"], 4)
        # the install that ran out of memory is fixed now and has the most boots
        self.post(submission(install="1" * 32, boots=12))
        self.assertEqual(self.run_sync(), "0 new plans, 1 updated: CUSA17419 ps5")
        self.assertEqual(a.read_bytes(), make_plan(boots=12))
        self.assertEqual(self.index()["plans"][0]["contributors"], 4)    # the same four installs
        self.assertEqual(self.index()["plans"][1]["path"], f"plans/LAPY10010/{FP_B}-ps4.bin")

    def test_main(self) -> None:
        key = Path(self.tmp.name) / "admin.key"
        key.write_text(ADMIN_KEY + "\n")
        self.post(submission())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = sync.main(["--server", self.url, "--repo", str(self.repo), "--state", str(self.state),
                              "--admin-key-file", str(key)])
        self.assertEqual((code, out.getvalue()), (0, "1 new plan, 0 updated: CUSA17419 ps5\n"))
        key.write_text(CONTRIB_KEY)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.main(["--server", self.url, "--repo", str(self.repo), "--state",
                                        str(self.state), "--admin-key-file", str(key)]), 1)


if __name__ == "__main__":
    unittest.main()
