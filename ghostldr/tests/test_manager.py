"""The web manager (ghostldr/lua/manager.lua) against a fake console.

manager_fake.lua stands in for Luac0re, libkernel, SWRR's save service and
the browser's connections, and runs the real manager.lua end to end; these
tests write its scenario and check the HTTP replies, the archives and what is
left in the fake save area.  Needs a desktop Lua 5.3 or 5.4 (`lua`); without
one only the script-size check runs.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import io
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
MANAGER = ROOT / "ghostldr/lua/manager.lua"
FAKE = Path(__file__).resolve().parent / "manager_fake.lua"
sys.path.insert(0, str(ROOT / "ghostldr/host"))
import gfs_pack as gp  # noqa: E402


def find_lua() -> str | None:
    for name in ("lua5.4", "lua5.3", "lua", "lua54", "lua53"):
        exe = shutil.which(name)
        if exe:
            out = subprocess.run([exe, "-v"], capture_output=True, text=True)
            if "Lua 5.3" in out.stdout + out.stderr or "Lua 5.4" in out.stdout + out.stderr:
                return exe
    return None


LUA = find_lua()
TOKEN = "{{TOKEN}}"            # manager_fake.lua puts the session token here


def lua_str(data: bytes | str) -> str:
    if isinstance(data, str):
        data = data.encode()
    return '"' + "".join(f"\\{b:03d}" for b in data) + '"'


def lua_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, (str, bytes)):
        return lua_str(v)
    if isinstance(v, dict):
        return "{" + ",".join(f"[{lua_str(k)}]={lua_value(x)}" for k, x in v.items()) + "}"
    if isinstance(v, (list, tuple)):
        return "{" + ",".join(lua_value(x) for x in v) + "}"
    raise TypeError(v)


def request(method: str, target: str, body: bytes = b"", token: str | None = None) -> bytes:
    head = [f"{method} {target} HTTP/1.1", "Host: 192.0.2.10:8090", "User-Agent: test"]
    if token:
        head.append(f"X-Ghostldr-Token: {token}")
    if method == "POST":
        head.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


def parse_response(raw: bytes) -> tuple[int, dict, bytes]:
    if not raw:
        return 0, {}, b""
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    headers = {k.lower(): v.strip() for k, v in (line.split(":", 1) for line in lines[1:])}
    return status, headers, body


def make_sfo(values: dict[str, str | int]) -> bytes:
    keys, data, entries = b"", b"", []
    for k, v in values.items():
        if isinstance(v, int):
            raw, fmt = struct.pack("<I", v), 0x0404
        else:
            raw, fmt = v.encode() + b"\0", 0x0204
        mx = (len(raw) + 3) & ~3
        entries.append(struct.pack("<HHIII", len(keys), fmt, len(raw), mx, len(data)))
        keys += k.encode() + b"\0"
        data += raw.ljust(mx, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    kt = 20 + 16 * len(entries)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, kt, kt + len(keys), len(entries)) + b"".join(entries) + keys + data


def tar_bytes(files: dict[str, bytes], fmt=tarfile.USTAR_FORMAT, dirs: list[str] = (), extra=None) -> bytes:
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=fmt) as tf:
        for d in dirs:
            ti = tarfile.TarInfo(d)
            ti.type = tarfile.DIRTYPE
            tf.addfile(ti)
        for name, data in files.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mtime = 1700000000
            if extra:
                extra(ti)
            tf.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


@mock.patch.object(gp, "CAP", 4096)
def ghost_install(tmp: Path, title: str = "CUSA99999", sfo: bool = True, prefix: str = "GHOST",
                  index: str = "GFSINDEX") -> list[dict]:
    """A tiny ghostfs install (index + data containers), with a param.sfo unless sfo=False."""
    tmp = tmp / f"{title}-{prefix}"
    src = tmp / "app0"
    shutil.rmtree(tmp, ignore_errors=True)
    (src / "sce_sys").mkdir(parents=True)
    (src / "eboot.bin").write_bytes(b"E" * 5000)
    if sfo:
        (src / "sce_sys/param.sfo").write_bytes(make_sfo({"TITLE": "Test Title", "APP_VER": "01.05",
                                                          "TITLE_ID": title}))
    (src / "data").mkdir()
    (src / "data/a.bin").write_bytes(b"A" * 3000)
    info = gp.pack(src, title, prefix, tmp / "out")
    out = [{"name": index, "blocks": 512, "files": {"index.bin": (tmp / "out/index.bin").read_bytes()}}]
    for c in info["containers"]:
        data = b"".join((src / rel).read_bytes()[off:off + n] for rel, off, n in c["pieces"])
        out.append({"name": c["dir"], "blocks": 32768, "files": {"data.bin": data}})
    return out


CAT_HEADER = struct.Struct("<4sIIIIIQ32x")              # docs/ghostfs-catalogue.md (hle/title-agnostic)
CAT_ENTRY = struct.Struct("<16s16s32s64s16sQQIIQ16x")


def catalogue(entries: list[dict], generation: int = 5) -> bytes:
    out = CAT_HEADER.pack(b"GFSC", 1, 64, 192, len(entries), 0, generation)
    for e in sorted(entries, key=lambda e: e["title"]):
        out += CAT_ENTRY.pack(e["title"].encode(), e["prefix"].encode(), e["index"].encode(), e.get("name", "").encode(),
                              e.get("version", "").encode(), 0x1122334455667788, e.get("total", 8124),
                              e.get("containers", 2), e.get("flags", 0), 1700000000)
    return out


def parse_catalogue(data: bytes) -> tuple[int, list[tuple]]:
    magic, _, hsize, esize, count, _, gen = CAT_HEADER.unpack_from(data)
    assert magic == b"GFSC" and len(data) == hsize + count * esize
    return gen, [CAT_ENTRY.unpack_from(data, hsize + i * esize) for i in range(count)]


SWRR_SAVE = {"name": "SLUS-20268", "blocks": 586, "files": {"VMC0.card": b"\x01" * 9000, "lua/init.lua": b"-- lua\n"}}
GHOSTSAVE = {"name": "GHOSTSAVE", "blocks": 256, "files": {"sdmemory_0.bin": bytes(range(256)) * 20,
                                                          "sdmemory_0.param": b"p" * 100}}


@unittest.skipUnless(LUA, "needs a desktop Lua 5.3/5.4")
class ManagerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_manager(self, containers: list[dict], requests: list, scratch: str | None = "GLDRT", **extra):
        reqs = []
        for r in requests + [request("POST", "/api/exit")]:
            reqs.append(r if isinstance(r, dict) else {"data": r})
        scenario = {"title": "CUSA03474", "containers": containers, "requests": reqs, **extra}
        if scratch:
            scenario["scratch"] = scratch
        sc = self.dir / "scenario.lua"
        sc.write_text("return " + lua_value(scenario))
        out = self.dir / "out"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        p = subprocess.run([LUA, str(FAKE), str(MANAGER), str(sc), str(out)], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        report = json.loads((out / "report.json").read_text())
        self.assertIsNone(report.get("error"), report.get("error"))
        resps = [parse_response((out / f"resp_{i + 1:03d}.bin").read_bytes()) for i in range(len(reqs))]
        # every request leaves the console clean, whatever happened
        self.assertEqual(report["violations"], [])
        self.assertEqual(report["still_mounted"], [])
        self.assertEqual(report["open_fds"], 0)
        self.assertEqual(report["temp_files"], {})
        self.assertEqual(report["unclosed_connections"], 0)
        self.assertEqual(resps[-1][0], 200)          # exit
        return report, resps[:-1]

    @staticmethod
    def files(report: dict, name: str) -> dict[str, bytes]:
        c = report["containers"][name]
        return {p: bytes.fromhex(h) for p, h in c["files"].items()}

    def test_page_and_state(self) -> None:
        cs = [SWRR_SAVE, GHOSTSAVE, *ghost_install(self.dir), {"name": "GLDRTA1", "files": {"x": b"1"}},
              {"name": "G11111SAVE"}, {"name": "ODDNAME"}]
        report, (page, state) = self.run_manager(cs, [request("GET", "/"), request("GET", "/api/state")])
        self.assertEqual(page[0], 200)
        self.assertIn(b"ghostldr manager", page[2])
        self.assertIn("DENY", page[1]["x-frame-options"])
        s = json.loads(state[2])
        self.assertEqual(len(s["titles"]), 1)
        t = s["titles"][0]
        self.assertEqual((t["id"], t["name"], t["version"], t["prefix"], t["index"]),
                         ("CUSA99999", "Test Title", "01.05", "GHOST", "GFSINDEX"))
        self.assertTrue(t["complete"])
        self.assertEqual(t["present"], t["containers"])
        self.assertEqual([x["name"] for x in t["saves"]], ["GHOSTSAVE"])
        self.assertEqual(s["swrr_save"]["name"], "SLUS-20268")
        self.assertTrue(s["swrr_save"]["readonly"])
        self.assertEqual([o["prefix"] for o in s["orphans"]], ["G11111"])
        self.assertEqual([x["name"] for x in s["scratch"]["saves"]], ["GLDRTA1"])
        self.assertEqual([o["name"] for o in s["other"]], ["ODDNAME"])
        self.assertEqual(s["swrr"]["save"], "SLUS-20268")
        # the URL and the token went to the log and the PS5's screen
        token = [line for line in report["logs"] if " token " in line][0].split()[-1]
        self.assertRegex(token, r"^[A-Z0-9]{4}-[A-Z0-9]{4}$")
        self.assertIn(token, report["notes"][0])
        self.assertIn("http://192.0.2.10:8090", report["notes"][0])
        # reading never mounted SWRR's own save and wrote nothing
        self.assertFalse([c for c in report["calls"] if c["op"] in ("delete",) or (c["op"] == "mount" and c["extra"] != 1)])

    def test_title_names_without_a_param_sfo(self) -> None:
        cs = ghost_install(self.dir, "CUSA99999", sfo=False)
        _, (state,) = self.run_manager(cs, [request("GET", "/api/state")],
                                       titles={"CUSA99999": {"name": "From a profile", "version": "9.99"}})
        t = json.loads(state[2])["titles"][0]
        self.assertEqual((t["name"], t["version"]), ("From a profile", "9.99"))
        cs = ghost_install(self.dir, "CUSA11456", sfo=False)
        _, (state,) = self.run_manager(cs, [request("GET", "/api/state")])
        self.assertEqual(json.loads(state[2])["titles"][0]["name"], "Ghost of Tsushima")

    def two_titles(self, flags: int = 0) -> list[dict]:
        ghost = ghost_install(self.dir, "CUSA11456", sfo=False)
        other = ghost_install(self.dir, "CUSA12345", prefix="G12345", index="G12345INDEX")
        cat = catalogue([
            {"title": "CUSA11456", "prefix": "GHOST", "index": "GFSINDEX", "name": "Ghost of Tsushima",
             "version": "02.18", "flags": 2},
            {"title": "CUSA12345", "prefix": "G12345", "index": "G12345INDEX", "name": "Second Game",
             "version": "01.00", "flags": flags}])
        return [SWRR_SAVE, GHOSTSAVE, *ghost, *other, {"name": "G12345SAVE", "files": {"s": b"progress"}},
                {"name": "GFSCATALOG", "blocks": 512, "files": {"catalog.bin": cat}}, {"name": "GFSFREE03"}]

    def test_catalogue_titles_side_by_side(self) -> None:
        _, (state,) = self.run_manager(self.two_titles(), [request("GET", "/api/state")])
        s = json.loads(state[2])
        titles = {t["id"]: t for t in s["titles"]}
        self.assertEqual(sorted(titles), ["CUSA11456", "CUSA12345"])
        g, o = titles["CUSA11456"], titles["CUSA12345"]
        self.assertEqual((g["name"], g["version"], g["index"], g["status"]), ("Ghost of Tsushima", "02.18", "GFSINDEX", "complete"))
        self.assertEqual((o["name"], o["version"], o["index"], o["status"]), ("Second Game", "01.00", "G12345INDEX", "complete"))
        self.assertTrue(g["catalogued"] and o["catalogued"])
        self.assertEqual([x["name"] for x in o["saves"]], ["G12345SAVE"])
        self.assertEqual((s["catalogue"]["titles"], s["catalogue"]["generation"]), (2, 5))
        self.assertEqual([(x["name"], x.get("deletable")) for x in s["other"]], [("GFSFREE03", True)])

    def test_catalogue_entry_marked_incomplete(self) -> None:
        _, (state,) = self.run_manager(self.two_titles(flags=1), [request("GET", "/api/state")])
        o = [t for t in json.loads(state[2])["titles"] if t["prefix"] == "G12345"][0]
        self.assertFalse(o["complete"])
        self.assertIn("interrupted", o["status"])

    def test_delete_title_follows_the_catalogue_order(self) -> None:
        cs = self.two_titles()
        report, (ok,) = self.run_manager(cs, [
            request("POST", "/api/delete-title?prefix=G12345&confirm=CUSA12345", token=TOKEN)])
        self.assertEqual(ok[0], 200, ok[2])
        ops = [(c["op"], c["name"], c.get("extra")) for c in report["calls"] if c["op"] in ("mount", "delete")]
        marked = ops.index(("mount", "GFSCATALOG", 2))
        first_delete = next(i for i, o in enumerate(ops) if o[0] == "delete")
        self.assertLess(marked, first_delete)
        self.assertEqual(ops[first_delete][1], "G12345INDEX")
        self.assertEqual(ops[-1], ("mount", "GFSCATALOG", 2))          # the entry dropped last
        gen, entries = parse_catalogue(self.files(report, "GFSCATALOG")["catalog.bin"])
        self.assertEqual(gen, 7)
        self.assertEqual([e[0].rstrip(b"\0") for e in entries], [b"CUSA11456"])
        left = sorted(report["containers"])
        self.assertIn("G12345SAVE", left)
        self.assertFalse([n for n in left if n.startswith("G12345") and n != "G12345SAVE"])
        self.assertIn("GFSINDEX", left)

    def test_shared_containers_are_not_save_targets(self) -> None:
        report, resps = self.run_manager(self.two_titles(), [
            request("POST", "/api/delete?c=GFSCATALOG&confirm=GFSCATALOG", token=TOKEN),
            request("POST", "/api/delete?c=G12345INDEX&confirm=G12345INDEX", token=TOKEN),
            request("POST", "/api/restore?c=GFSCATALOG&confirm=GFSCATALOG", tar_bytes({"a": b"1"}), token=TOKEN),
            request("POST", "/api/delete?c=GFSFREE03&confirm=GFSFREE03", token=TOKEN)])
        self.assertEqual([r[0] for r in resps], [403, 403, 403, 200])
        self.assertIn("GFSCATALOG", report["containers"])
        self.assertNotIn("GFSFREE03", report["containers"])

    def test_incomplete_and_partial_installs(self) -> None:
        cs = ghost_install(self.dir)
        idx = bytearray(cs[0]["files"]["index.bin"])
        struct.pack_into("<I", idx, 60, 1)              # install interrupted
        cs[0]["files"]["index.bin"] = bytes(idx)
        del cs[2]                                        # GHOST001 missing
        cs.append({"name": "G22222000", "blocks": 32768, "files": {"data.bin": b"x"}})
        _, (state,) = self.run_manager(cs, [request("GET", "/api/state")])
        titles = {t["prefix"]: t for t in json.loads(state[2])["titles"]}
        self.assertFalse(titles["GHOST"]["complete"])
        self.assertEqual(titles["GHOST"]["status"], "install interrupted")
        self.assertEqual(titles["GHOST"]["missing"], ["GHOST001"])
        self.assertEqual(titles["G22222"]["status"], "leftover data without an index")

    def test_download_is_a_tar_with_a_manifest(self) -> None:
        save = {"name": "GHOSTSAVE", "blocks": 300, "files": {"sdmemory_0.bin": b"\xAA" * 70000,
                                                             "sub/deeper/f.txt": b"hello", "empty.bin": b""}}
        _, (dl,) = self.run_manager([SWRR_SAVE, save], [request("GET", "/api/download?c=GHOSTSAVE")])
        status, headers, body = dl
        self.assertEqual(status, 200)
        self.assertEqual(int(headers["content-length"]), len(body))
        self.assertRegex(headers["content-disposition"], r'filename="GHOSTSAVE-\d{8}-\d{6}\.tar"')
        with tarfile.open(fileobj=io.BytesIO(body)) as tf:
            names = tf.getnames()
            self.assertEqual(names[0], "ghostldr-save.json")
            man = json.loads(tf.extractfile("ghostldr-save.json").read())
            self.assertEqual((man["container"], man["blocks"], man["kind"], man["prefix"]),
                             ("GHOSTSAVE", 300, "progress", "GHOST"))
            self.assertEqual(tf.extractfile("GHOSTSAVE/sdmemory_0.bin").read(), b"\xAA" * 70000)
            self.assertEqual(tf.extractfile("GHOSTSAVE/sub/deeper/f.txt").read(), b"hello")
            self.assertEqual(tf.extractfile("GHOSTSAVE/empty.bin").read(), b"")
            self.assertIn("GHOSTSAVE/sce_sys/param.sfo", names)
            self.assertTrue(tf.getmember("GHOSTSAVE/sub").isdir())

    def test_swrr_save_is_read_from_savedata0_and_never_written(self) -> None:
        token = TOKEN
        report, (dl, d1, d2, rs, cp) = self.run_manager([SWRR_SAVE, GHOSTSAVE], [
            request("GET", "/api/download?c=SLUS-20268"),
            request("POST", "/api/delete?c=SLUS-20268&confirm=SLUS-20268", token=token),
            request("POST", "/api/delete-title?prefix=SLUS-20268&confirm=SLUS-20268", token=token),
            request("POST", "/api/restore?c=SLUS-20268&confirm=SLUS-20268", tar_bytes({"a": b"1"}), token=token),
            request("POST", "/api/copy?from=GHOSTSAVE&to=SLUS-20268&confirm=SLUS-20268", token=token)])
        self.assertEqual(dl[0], 200)
        with tarfile.open(fileobj=io.BytesIO(dl[2])) as tf:
            self.assertEqual(tf.extractfile("SLUS-20268/VMC0.card").read(), b"\x01" * 9000)
        self.assertEqual([d1[0], d2[0], rs[0], cp[0]], [403, 404, 403, 403])
        self.assertFalse([c for c in report["calls"] if c["name"] == "SLUS-20268"])
        self.assertEqual(self.files(report, "SLUS-20268")["VMC0.card"], b"\x01" * 9000)

    def test_writes_need_the_token(self) -> None:
        report, (no, wrong, auth_ok, ok) = self.run_manager([{"name": "GLDRTA1", "files": {"x": b"1"}}], [
            request("POST", "/api/delete?c=GLDRTA1&confirm=GLDRTA1"),
            request("POST", "/api/delete?c=GLDRTA1&confirm=GLDRTA1", token="AAAA-AAAA"),
            request("POST", "/api/auth", token=TOKEN.lower()),
            request("POST", "/api/delete?c=GLDRTA1&confirm=GLDRTA1", token=TOKEN)])
        self.assertEqual([no[0], wrong[0], auth_ok[0], ok[0]], [403, 403, 200, 200])
        self.assertNotIn("GLDRTA1", report["containers"])
        self.assertEqual([c["name"] for c in report["calls"] if c["op"] == "delete"], ["GLDRTA1"])

    def test_wrong_tokens_slow_down_then_lock(self) -> None:
        reqs = [request("POST", "/api/auth", token="WRONG-0000") for _ in range(6)]
        _, resps = self.run_manager([], reqs + [request("POST", "/api/auth", token=TOKEN)])
        self.assertEqual([r[0] for r in resps[:5]], [403] * 5)
        self.assertEqual(resps[5][0], 429)            # waiting after five
        self.assertEqual(resps[6][0], 429)            # even the right token waits

    def test_restore_round_trip_into_a_new_container(self) -> None:
        src = {"name": "GLDRTA1", "blocks": 200, "files": {"save.dat": b"S" * 40000, "dir/b.bin": b"B" * 10}}
        report, (dl,) = self.run_manager([src], [request("GET", "/api/download?c=GLDRTA1")])
        archive = dl[2]
        report, (chk, rs, again) = self.run_manager([src], [
            request("POST", "/api/check-restore?c=GLDRTB2&need=10", token=TOKEN),
            request("POST", "/api/restore?c=GLDRTB2&confirm=GLDRTB2", archive, token=TOKEN),
            request("POST", "/api/restore?c=GLDRTB2&confirm=GLDRTB2&strip=GLDRTA1", archive, token=TOKEN)])
        c = json.loads(chk[2])
        self.assertTrue(c["fits"])
        self.assertFalse(c["exists"])
        r = json.loads(rs[2])
        self.assertEqual((rs[0], r["files"], r["created"], r["blocks"]), (200, 2, True, 200))
        self.assertEqual(r["skipped"], ["sce_sys", "sce_sys/param.sfo"])
        self.assertEqual(again[0], 200)
        self.assertFalse(json.loads(again[2])["created"])
        got = self.files(report, "GLDRTB2")
        self.assertEqual(got["save.dat"], b"S" * 40000)
        self.assertEqual(got["dir/b.bin"], b"B" * 10)
        self.assertEqual(report["containers"]["GLDRTB2"]["blocks"], 200)
        self.assertIn("sce_sys/param.sfo", got)        # made by the system, never from the archive

    def test_restore_replaces_files_and_keeps_system_files(self) -> None:
        dst = {"name": "GHOSTSAVE", "blocks": 256, "files": {"old.bin": b"old", "keep/x": b"x"}}
        report, (rs,) = self.run_manager([dst], [
            request("POST", "/api/restore?c=GHOSTSAVE&confirm=GHOSTSAVE",
                    tar_bytes({"new.bin": b"N" * 5, "sce_sys/param.sfo": b"evil", "._new.bin": b"mac"}), token=TOKEN)])
        self.assertEqual(rs[0], 200, rs[2])
        got = self.files(report, "GHOSTSAVE")
        self.assertEqual(sorted(got), ["new.bin", "sce_sys/param.sfo"])
        self.assertEqual(got["sce_sys/param.sfo"], b"P" * 1024)
        self.assertEqual(json.loads(rs[2])["skipped"], ["sce_sys/param.sfo", "._new.bin"])

    def test_restore_accepts_pax_and_gnu_archives(self) -> None:
        long = "d" * 70 + "/" + "f" * 60 + ".bin"            # > 100 characters: pax / GNU long names
        for fmt in (tarfile.PAX_FORMAT, tarfile.GNU_FORMAT):
            archive = tar_bytes({"GLDRTS/" + long: b"L" * 3, "GLDRTS/plain": b"p"}, fmt=fmt, dirs=["GLDRTS"])
            report, (rs,) = self.run_manager([], [
                request("POST", "/api/restore?c=GLDRTC3&confirm=GLDRTC3&strip=GLDRTS", archive, token=TOKEN)])
            self.assertEqual(rs[0], 200, (fmt, rs[2]))
            self.assertEqual(self.files(report, "GLDRTC3")[long], b"L" * 3)

    def test_bad_archives_change_nothing(self) -> None:
        dst = {"name": "GLDRTA1", "files": {"keep": b"k"}}
        evil = tar_bytes({"../escape": b"x"})
        link = io.BytesIO()
        with tarfile.open(fileobj=link, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            ti = tarfile.TarInfo("ln")
            ti.type = tarfile.SYMTYPE
            ti.linkname = "/etc/passwd"
            tf.addfile(ti)
        good = tar_bytes({"a": b"1" * 1000})
        report, resps = self.run_manager([dst], [
            request("POST", "/api/restore?c=GLDRTA1&confirm=GLDRTA1", evil, token=TOKEN),
            request("POST", "/api/restore?c=GLDRTA1&confirm=GLDRTA1", link.getvalue(), token=TOKEN),
            request("POST", "/api/restore?c=GLDRTA1&confirm=GLDRTA1", b"not a tar" * 200, token=TOKEN),
            request("POST", "/api/restore?c=GLDRTA1", good, token=TOKEN),
            {"data": request("POST", "/api/restore?c=GLDRTA1&confirm=GLDRTA1", good, token=TOKEN)[:-500],
             "stall": True},
            request("POST", "/api/restore?c=GFSINDEX&confirm=GFSINDEX", good, token=TOKEN),
            request("POST", "/api/restore?c=GHOST000&confirm=GHOST000", good, token=TOKEN)])
        self.assertEqual([r[0] for r in resps], [400, 400, 400, 400, 400, 403, 403])
        self.assertIn("stopped early", json.loads(resps[4][2])["error"])
        self.assertEqual(self.files(report, "GLDRTA1")["keep"], b"k")
        self.assertFalse([c for c in report["calls"] if c["op"] == "mount" and c["extra"] != 1])

    def test_restore_too_big_for_the_container(self) -> None:
        dst = {"name": "GLDRTA1", "blocks": 96, "files": {"keep": b"k"}}
        report, (chk, rs) = self.run_manager([dst], [
            request("POST", "/api/check-restore?c=GLDRTA1&need=200", token=TOKEN),
            request("POST", "/api/restore?c=GLDRTA1&confirm=GLDRTA1", tar_bytes({"big": b"B" * (100 * 32768)}),
                    token=TOKEN)])
        self.assertFalse(json.loads(chk[2])["fits"])
        self.assertEqual(rs[0], 409)
        self.assertEqual(self.files(report, "GLDRTA1")["keep"], b"k")

    def test_restore_reports_no_space(self) -> None:
        report, (rs,) = self.run_manager([], [
            request("POST", "/api/restore?c=GLDRTN1&confirm=GLDRTN1", tar_bytes({"a": b"1"}), token=TOKEN)],
            no_space=True)
        self.assertEqual(rs[0], 409)
        self.assertIn("no space left", json.loads(rs[2])["error"])
        self.assertNotIn("GLDRTN1", report["containers"])

    def test_copy_within_a_title(self) -> None:
        cs = [{"name": "GHOSTSAVE", "blocks": 256, "files": {"sdmemory_0.bin": b"m" * 99}},
              {"name": "GLDRTA1", "files": {"a": b"aa", "d/b": b"bb"}}, {"name": "GLDRTB2", "files": {"old": b"o"}}]
        report, (ok, over, cross, prot) = self.run_manager(cs, [
            request("POST", "/api/copy?from=GLDRTA1&to=GLDRTC3&confirm=GLDRTC3", token=TOKEN),
            request("POST", "/api/copy?from=GLDRTA1&to=GLDRTB2&confirm=GLDRTB2", token=TOKEN),
            request("POST", "/api/copy?from=GHOSTSAVE&to=GLDRTD4&confirm=GLDRTD4", token=TOKEN),
            request("POST", "/api/copy?from=GLDRTA1&to=GHOSTSAVE&confirm=GHOSTSAVE", token=TOKEN)])
        self.assertEqual([ok[0], over[0], cross[0], prot[0]], [200, 200, 403, 403])
        self.assertEqual({p: d for p, d in self.files(report, "GLDRTC3").items() if not p.startswith("sce_sys")},
                         {"a": b"aa", "d/b": b"bb"})
        self.assertNotIn("old", self.files(report, "GLDRTB2"))
        self.assertEqual(self.files(report, "GHOSTSAVE")["sdmemory_0.bin"], b"m" * 99)

    def test_delete_title_keeps_its_saves_unless_asked(self) -> None:
        install = ghost_install(self.dir)
        cs = [SWRR_SAVE, GHOSTSAVE, *install, {"name": "GHOSTD001SAVE"}]
        report, (wrong, ok) = self.run_manager(cs, [
            request("POST", "/api/delete-title?prefix=GHOST&confirm=GHOST", token=TOKEN),
            request("POST", "/api/delete-title?prefix=GHOST&confirm=CUSA99999", token=TOKEN)])
        self.assertEqual(wrong[0], 400)
        self.assertEqual(ok[0], 200, ok[2])
        deleted = [c["name"] for c in report["calls"] if c["op"] == "delete"]
        self.assertEqual(deleted[0], "GFSINDEX")         # the index first: never a bootable half install
        self.assertEqual(sorted(deleted), sorted(c["name"] for c in install))
        self.assertEqual(sorted(report["containers"]), ["GHOSTD001SAVE", "GHOSTSAVE", "SLUS-20268"])
        report, (ok,) = self.run_manager(cs, [
            request("POST", "/api/delete-title?prefix=GHOST&confirm=CUSA99999&saves=1", token=TOKEN)])
        self.assertEqual(sorted(report["containers"]), ["SLUS-20268"])

    def test_vanished_browser_and_stalled_requests_do_not_stop_the_server(self) -> None:
        big = {"name": "GLDRTA1", "blocks": 300, "files": {"big": b"z" * 200000}}
        report, (dl, idle, after) = self.run_manager([big], [
            {"data": request("GET", "/api/download?c=GLDRTA1"), "close_after": 5000},
            {"data": b"GET / HT", "stall": True},
            request("GET", "/api/state")])
        self.assertEqual(dl[0], 200)
        self.assertEqual(idle[0], 0)                 # no reply to a request that never finished
        self.assertEqual(after[0], 200)

    def test_split_request_heads_and_unknown_routes(self) -> None:
        _, (split, nf, meth, junk) = self.run_manager([], [
            {"data": request("GET", "/api/state"), "chunk": 7},
            request("GET", "/nope"), request("PUT", "/api/state"), b"\x16\x03\x01 not http\r\n\r\n"])
        self.assertEqual([split[0], nf[0], meth[0], junk[0]], [200, 404, 405, 400])

    def test_busy_port_moves_to_the_next(self) -> None:
        report, _ = self.run_manager([], [], busy_ports=[8090, 8091])
        self.assertEqual(report["port"], 8092)
        self.assertIn("http://192.0.2.10:8092", report["notes"][0])

    def test_scratch_prefix_must_not_shadow_real_names(self) -> None:
        for bad in ("GHOST", "GFSI", "SLUS", "G12"):
            _, (state,) = self.run_manager([{"name": bad + "X1"}], [request("GET", "/api/state")], scratch=bad)
            self.assertIsNone(json.loads(state[2]).get("scratch"))


class ScriptTests(unittest.TestCase):
    def test_fits_luac0re_loader(self) -> None:
        size = MANAGER.stat().st_size
        self.assertLess(size + 4096, 500 * 1024)        # the loader reads at most 500 KB

    def test_page_has_no_lua_long_string_end(self) -> None:
        text = MANAGER.read_text()
        page = text[text.index("M.PAGE = [==["):text.index("]==]")]
        self.assertNotIn("]==]", page[13:])


if __name__ == "__main__":
    unittest.main()
