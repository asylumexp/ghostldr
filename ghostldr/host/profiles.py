#!/usr/bin/env python3
"""Per-title compatibility profiles (ghostldr/data/titles/*.json).

Everything ghostldr does differently for one title lives in its profile, as
data: how its files are packed, its sceAppContent parameters, its memory plan
(only for titles that need more than SWRR's budget gives them), title-specific
diagnostics, and HLE rules that fix single imports without writing C.
default.json holds the generic values; <TITLE>.json overrides them (objects
merge key by key, everything else replaces).  docs/compatibility.md explains
every field.

run.py compiles the merged profile into a small blob (compile_blob) and sends
it with the core as a GHOSTLDR_PROFILE_HEX line, so changing a profile needs
no reinstall.  The core's twin is ghostldr/native/src/profile.c.

    python3 ghostldr/host/profiles.py check                 # validate every profile
    python3 ghostldr/host/profiles.py show CUSA11456        # merged profile and its blob size
    python3 ghostldr/host/profiles.py detect --src DIR      # which title a dump is
    python3 ghostldr/host/profiles.py new --src DIR         # a starter profile for a dump
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLES = ROOT / "ghostldr/data/titles"
DEFAULT_SRC = ROOT / "build/ghost/app0"          # where the dump goes unless --src says otherwise
sys.path.insert(0, str(ROOT / "ghostldr/tools"))
import gfs_catalog as gc  # noqa: E402

TITLE_RE = re.compile(r"[A-Z0-9_-]{1,16}")
NID_RE = re.compile(r"[A-Za-z0-9+-]{11}")

# feature names -> core flag bits (profile.c PF_*)
FEATURES = {
    "ghost_hooks": 1 << 0,        # hooks.c: inline diagnostic hooks at fixed eboot offsets
    "ghost_diagnostics": 1 << 1,  # crash.c / hle.c: watchdog reads of globals at fixed eboot offsets
    "trace_opens": 1 << 2,        # log every file open (capture_order.py turns run.log into an order file)
}
# features whose C code was written against one title's executable
FEATURE_TITLE = {"ghost_hooks": "CUSA11456", "ghost_diagnostics": "CUSA11456"}
# run.py --gpu-heap-mib/--pool1-mib/--cpu-heap-mib -> the boot_ctx reserved[] slot
# the core reads them from (they override a plan table's mib; --slack-mib, slot
# 5, overrides memory.runtime_reserve_mib)
KNOBS = {"gpu_heap": 4, "pool1": 6, "cpu_heap": 7}
NO_KNOB = 0xFF
ROLES = {"set": 0, "absorb": 1}
ACTIONS = {"return": 1, "trace": 2}

# blob layout, shared with profile.c
MAGIC = b"GLPF"
VERSION = 1
HEADER = struct.Struct("<4sHHII16s20sI5iIIIII")   # 96 bytes
REC_PLAN, REC_TABLE, REC_RULE, REC_MODULE = 1, 2, 3, 4
MAX_BLOB = 8192
MAX_RULES = 64


class ProfileError(Exception):
    pass


# ---- the title's own files ----

def param_sfo(src: Path | None) -> dict:
    """Every key of sce_sys/param.sfo; {} when it is missing or unreadable
    (homebrew often ships a minimal or odd one)."""
    try:
        d = (Path(src) / "sce_sys/param.sfo").read_bytes()
        if d[:4] != b"\0PSF":
            return {}
        kt, dt, n = struct.unpack_from("<III", d, 8)
        out = {}
        for i in range(min(n, 256)):
            ko, fmt, ln, _, do = struct.unpack_from("<HHIII", d, 20 + 16 * i)
            key = d[kt + ko:d.index(b"\0", kt + ko)].decode("ascii", "replace")
            raw = d[dt + do:dt + do + ln]
            if fmt == 0x0404 and ln >= 4:
                out[key] = struct.unpack_from("<i", raw)[0]
            else:
                out[key] = raw.split(b"\0")[0].decode("utf-8", "replace")
        return out
    except (OSError, ValueError, struct.error):
        return {}


def eboot_fingerprint(src: Path | None) -> str | None:
    """DT_SCE_FINGERPRINT of app0/eboot.bin (20 bytes, hex): names one build of
    a title whatever tool made the dump.  None for a plain ELF without one."""
    try:
        import orbis_elf as oe
        elf = oe.unwrap_self((Path(src) / "eboot.bin").read_bytes())
        phoff, = struct.unpack_from("<Q", elf, 0x20)
        phent, phnum = struct.unpack_from("<HH", elf, 0x36)
        phdrs = [struct.unpack_from("<IIQQQQQQ", elf, phoff + i * phent) for i in range(phnum)]
        dyn = next((p for p in phdrs if p[0] == oe.PT_DYNAMIC), None)
        data = next((p for p in phdrs if p[0] == oe.PT_SCE_DYNLIBDATA), None)
        if not dyn or not data:
            return None
        for i in range(dyn[5] // 16):
            tag, val = struct.unpack_from("<qQ", elf, dyn[2] + 16 * i)
            if tag == 0:
                break
            if tag == oe.DT_SCE_FINGERPRINT:
                fp = elf[data[2] + val:data[2] + val + 20]
                return fp.hex() if len(fp) == 20 and any(fp) else None
    except (OSError, ValueError, struct.error, ImportError):
        pass
    return None


def normalize_title(text: str | None) -> str | None:
    t = (text or "").strip().upper()
    return t if TITLE_RE.fullmatch(t) else None


def default_prefix(title: str) -> str:
    """Container prefix for a title without one in its profile: G + the
    digits of a normal ID (CUSA12345 -> G12345), else G + its first 11 letters
    and digits.  The prefix also names the title's progress container
    (<prefix>SAVE), so it must not change once a title has been played."""
    if re.fullmatch(r"[A-Z]{4}\d{5}", title):
        return "G" + title[4:]
    return ("G" + re.sub(r"[^A-Z0-9]", "", title))[:12]


def known_titles() -> list[str]:
    return sorted(p.stem for p in TITLES.glob("*.json") if p.stem != "default")


def detect_title(src: Path | None) -> str | None:
    """The dump's title ID: param.sfo's TITLE_ID, else the profile whose eboot
    fingerprint matches (dumps without sce_sys/param.sfo)."""
    t = normalize_title(str(param_sfo(src).get("TITLE_ID", "")))
    if t:
        return t
    fp = eboot_fingerprint(src) if src else None
    if fp:
        for title in known_titles():
            if raw_profile(title).get("eboot", {}).get("fingerprint", "").lower() == fp:
                return title
    return None


# ---- loading ----

def raw_profile(name: str) -> dict:
    path = TITLES / f"{name}.json"
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError as e:
        raise ProfileError(f"{path.relative_to(ROOT)}: {e}") from None


def merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in over.items():
        out[k] = merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else copy.deepcopy(v)
    return out


def load(title: str | None) -> dict:
    """default.json merged with <title>.json (validated).  A title without a
    profile gets the generic defaults."""
    prof = raw_profile("default")
    own = raw_profile(title) if title else {}
    if own:
        if own.get("title") != title:
            raise ProfileError(f"ghostldr/data/titles/{title}.json: its \"title\" must be {title}")
        prof = merge(prof, own)
    prof["title"] = title or ""
    prof["_has_profile"] = bool(own)
    errors = validate(prof)
    if errors:
        where = f"ghostldr/data/titles/{title}.json" if own else "ghostldr/data/titles/default.json"
        raise ProfileError(f"{where}: " + "; ".join(errors))
    return prof


# ---- validation ----

def _int(v, lo=None, hi=None) -> int | None:
    """An int or a "0x..." string."""
    if isinstance(v, bool):
        return None
    if isinstance(v, str):
        try:
            v = int(v, 0)
        except ValueError:
            return None
    if not isinstance(v, int) or (lo is not None and v < lo) or (hi is not None and v > hi):
        return None
    return v


def _regex(errors: list, where: str, pattern) -> None:
    if not isinstance(pattern, str):
        errors.append(f"{where} must be a regular expression string")
        return
    try:
        re.compile(pattern)
    except re.error as e:
        errors.append(f"{where}: {e}")


def _keys(errors: list, where: str, obj, allowed: set, required: set = frozenset()) -> bool:
    if not isinstance(obj, dict):
        errors.append(f"{where} must be an object")
        return False
    for k in obj:
        if k not in allowed and not k.startswith("_"):
            errors.append(f"{where}: unknown key {k!r}")
    for k in required - set(obj):
        errors.append(f"{where}: {k!r} is required")
    return True


TOP_KEYS = {"title", "name", "version", "status", "notes", "eboot", "pack", "app_params", "save_blocks",
            "features", "memory", "modules", "hle"}
MEMORY_KEYS = {"mode", "runtime_reserve_mib", "video_reserve_mib", "flex_mib", "min_mib", "plan"}


def validate(p: dict) -> list[str]:
    e: list[str] = []
    if not _keys(e, "profile", p, TOP_KEYS):
        return e
    for k in ("name", "version", "status", "notes"):
        if k in p and not isinstance(p[k], str):
            e.append(f"{k} must be a string")
    if p.get("title") and not TITLE_RE.fullmatch(p["title"]):
        e.append(f"title {p['title']!r}: use A-Z 0-9 _ - (at most 16)")
    if "eboot" in p and _keys(e, "eboot", p["eboot"], {"fingerprint"}):
        fp = p["eboot"].get("fingerprint", "")
        if fp and not re.fullmatch(r"[0-9a-fA-F]{40}", fp):
            e.append("eboot.fingerprint must be 40 hex digits (profiles.py detect prints it)")
    pk = p.get("pack", {})
    if _keys(e, "pack", pk, {"prefix", "order", "late", "exclude", "groups"}):
        if "prefix" in pk:
            why = gc.prefix_problem(pk["prefix"]) if isinstance(pk["prefix"], str) else "it must be a string"
            if why:
                e.append(f"pack.prefix: {why}")
        if pk.get("order") is not None:
            if not isinstance(pk["order"], str) or not (TITLES / pk["order"]).is_file():
                e.append(f"pack.order: no file ghostldr/data/titles/{pk['order']}")
        if not isinstance(pk.get("late", []), list) or not all(isinstance(x, str) for x in pk.get("late", [])):
            e.append("pack.late must be a list of path prefixes")
        if "exclude" in pk:
            _regex(e, "pack.exclude", pk["exclude"])
        for i, g in enumerate(pk.get("groups") or []):
            if not (isinstance(g, list) and len(g) == 2 and _int(g[1], 0) is not None):
                e.append(f"pack.groups[{i}] must be [regex, rank >= 0]")
            else:
                _regex(e, f"pack.groups[{i}]", g[0])
    ap = p.get("app_params", {})
    if not isinstance(ap, dict) or any(k not in "01234" or len(k) != 1 or _int(v, -2**31, 2**31 - 1) is None
                                       for k, v in ap.items()):
        e.append('app_params must map "0".."4" to 32-bit integers')
    if "save_blocks" in p and _int(p["save_blocks"], 32, 32768) is None:
        e.append("save_blocks must be 32..32768 (32 KiB blocks)")
    feats = p.get("features", [])
    if not isinstance(feats, list) or any(f not in FEATURES for f in feats):
        e.append(f"features: known ones are {', '.join(FEATURES)}")
    else:
        for f in feats:
            if FEATURE_TITLE.get(f, p.get("title")) != p.get("title"):
                e.append(f"feature {f}: its C code is written for {FEATURE_TITLE[f]}'s executable only")
    m = p.get("memory", {})
    if _keys(e, "memory", m, MEMORY_KEYS):
        if m.get("mode", "transparent") not in ("transparent", "plan"):
            e.append('memory.mode is "transparent" (plan-free) or "plan"')
        for k in MEMORY_KEYS - {"mode", "plan"}:
            if k in m and _int(m[k], 0, 8192) is None:
                e.append(f"memory.{k} must be 0..8192 MiB")
        if m.get("mode") == "plan":
            _validate_plan(e, m.get("plan"))
            if not p.get("eboot", {}).get("fingerprint"):
                e.append("a memory plan patches fixed eboot offsets: set eboot.fingerprint")
        elif "plan" in m:
            e.append('memory.plan is only used with "mode": "plan"')
    mods = p.get("modules", [])
    if not isinstance(mods, list) or any(not (isinstance(x, str) and re.fullmatch(r"[\w.+-]+\.s?prx", x))
                                         for x in mods):
        e.append("modules must be a list of sce_module file names (*.prx)")
    rules = p.get("hle", [])
    if not isinstance(rules, list):
        e.append("hle must be a list of rules")
    else:
        if len(rules) > MAX_RULES:
            e.append(f"at most {MAX_RULES} hle rules")
        for i, r in enumerate(rules):
            _validate_rule(e, f"hle[{i}]", r)
    return e


def _validate_plan(e: list, plan) -> None:
    if not _keys(e, "memory.plan", plan, {"title_fixed_mib", "overcommit_mib", "lazy_sites", "tables"},
                 {"title_fixed_mib", "tables"}):
        return
    for k in ("title_fixed_mib", "overcommit_mib"):
        if k in plan and _int(plan[k], 0, 65535) is None:
            e.append(f"memory.plan.{k} must be 0..65535 MiB")
    tables = plan["tables"]
    if not isinstance(tables, list) or not 1 <= len(tables) <= 8:
        e.append("memory.plan.tables must list 1-8 tables")
        return
    for i, t in enumerate(tables):
        w = f"memory.plan.tables[{i}]"
        if not _keys(e, w, t, {"table", "role", "what", "knob", "mib", "min_mib", "expect"}, {"table", "role"}):
            continue
        if _int(t["table"], 0, 2**32 - 8) is None:
            e.append(f"{w}.table must be an offset in the eboot's text")
        if t["role"] not in ROLES:
            e.append(f"{w}.role is set or absorb")
        if "knob" in t and t["knob"] not in KNOBS:
            e.append(f"{w}.knob: one of {', '.join(KNOBS)}")
        for k in ("mib", "min_mib"):
            if k in t and _int(t[k], 0, 65535) is None:
                e.append(f"{w}.{k} must be 0..65535")
        exp = t.get("expect", [])
        if not isinstance(exp, list) or len(exp) > 16 or any(_int(v, 0, 2**64 - 1) is None for v in exp):
            e.append(f"{w}.expect must list at most 16 u64 values (the table's contents)")
        if "what" in t and not isinstance(t["what"], str):
            e.append(f"{w}.what must be a string")
    absorb = [t for t in tables if isinstance(t, dict) and t.get("role") == "absorb"]
    if len(absorb) != 1 or not absorb[0].get("expect"):
        e.append("memory.plan needs exactly one absorb table, with expect (its contents, checked before patching)")
    sites = plan.get("lazy_sites", [])
    if not isinstance(sites, list) or len(sites) > 16 or any(_int(s, 0, 2**32 - 1) is None for s in sites):
        e.append("memory.plan.lazy_sites must list at most 16 eboot offsets")


def _validate_rule(e: list, where: str, r) -> None:
    if not _keys(e, where, r, {"lib", "function", "nid", "action", "value", "fill", "log", "note"}):
        return
    if not (r.get("function") or r.get("nid")):
        e.append(f"{where}: needs a function name or a nid")
    if "function" in r and not (isinstance(r["function"], str) and re.fullmatch(r"[\w.:$@?]+", r["function"])):
        e.append(f"{where}.function must be a symbol name")
    if "nid" in r and not (isinstance(r["nid"], str) and NID_RE.fullmatch(r["nid"])):
        e.append(f"{where}.nid must be an 11-character NID")
    if "lib" in r and not (isinstance(r["lib"], str) and re.fullmatch(r"\w{1,63}", r["lib"])):
        e.append(f"{where}.lib must be a library name (e.g. libSceNpManager)")
    if r.get("action", "return") not in ACTIONS:
        e.append(f"{where}.action is return or trace")
    if "value" in r and _int(r["value"], -2**63, 2**64 - 1) is None:
        e.append(f"{where}.value must be an integer")
    if "log" in r and _int(r["log"], 0, 65535) is None:
        e.append(f"{where}.log must be 0..65535 (calls to log)")
    fills = r.get("fill", [])
    if not isinstance(fills, list) or len(fills) > 8:
        e.append(f"{where}.fill must list at most 8 writes")
        return
    if fills and r.get("action") == "trace":
        e.append(f"{where}: a trace rule cannot fill (it passes the call on)")
    for j, f in enumerate(fills):
        w = f"{where}.fill[{j}]"
        if not _keys(e, w, f, {"arg", "offset", "size", "value", "zero"}, {"arg"}):
            continue
        if _int(f["arg"], 1, 6) is None:
            e.append(f"{w}.arg must be 1..6 (the pointer parameter)")
        if _int(f.get("offset", 0), 0, 65535) is None:
            e.append(f"{w}.offset must be 0..65535")
        if "zero" in f:
            if _int(f["zero"], 1, 65535) is None or "value" in f or "size" in f:
                e.append(f"{w}: zero takes a byte count (1..65535) and no value/size")
        elif f.get("size", 4) not in (1, 2, 4, 8) or _int(f.get("value", 0), -2**63, 2**64 - 1) is None:
            e.append(f"{w}: size 1/2/4/8 and an integer value (or zero: N)")


# ---- compiling ----

def app_params(prof: dict, src: Path | None) -> list[int]:
    """sceAppContentAppParamGetInt values for ids 0-4: SKU (3 = full game),
    then USER_DEFINED_PARAM_1-4 from the dump's param.sfo; the profile's
    app_params win."""
    sfo = param_sfo(src) if src else {}
    vals = [3] + [int(sfo.get(f"USER_DEFINED_PARAM_{i}", 0) or 0) for i in range(1, 5)]
    for k, v in prof.get("app_params", {}).items():
        vals[int(k)] = _int(v)
    return [((v + 2**31) % 2**32) - 2**31 for v in vals]


def _rec(kind: int, payload: bytes) -> bytes:
    payload += b"\0" * (-(len(payload) + 4) % 8)
    return struct.pack("<HH", kind, len(payload) + 4) + payload


def _cstr(s: str) -> bytes:
    return s.encode() + b"\0"


def compile_blob(prof: dict, src: Path | None = None, extra_features: tuple[str, ...] = ()) -> bytes:
    import orbis_elf as oe
    flags = 0
    for f in list(prof.get("features", [])) + list(extra_features):
        flags |= FEATURES[f]
    fp = bytes.fromhex(prof.get("eboot", {}).get("fingerprint", "") or "00" * 20)
    mem = prof.get("memory", {})
    recs = b""
    if mem.get("mode") == "plan":
        plan = mem["plan"]
        sites = [_int(x) for x in plan.get("lazy_sites", [])]
        recs += _rec(REC_PLAN, struct.pack(f"<3I{len(sites)}I", _int(plan["title_fixed_mib"]),
                                           _int(plan.get("overcommit_mib", 0)), len(sites), *sites))
        for t in plan["tables"]:
            exp = [_int(v) for v in t.get("expect", [])]
            recs += _rec(REC_TABLE, struct.pack("<IBBHIII", _int(t["table"]), ROLES[t["role"]],
                                                KNOBS.get(t.get("knob"), NO_KNOB), len(exp), _int(t.get("mib", 0)),
                                                _int(t.get("min_mib", 0)), 0)
                         + struct.pack(f"<{len(exp)}Q", *exp) + _cstr(t.get("what", "table")[:40]))
    for r in prof.get("hle", []):
        nid = r.get("nid") or oe.compute_nid(r["function"])
        fills = r.get("fill", [])
        body = struct.pack("<BBH12sIQQ", ACTIONS[r.get("action", "return")], len(fills), _int(r.get("log", 1)),
                           nid.encode(), 0, _int(r.get("value", 0)) % 2**64, 0)
        for f in fills:
            arg, off = _int(f["arg"]) - 1, _int(f.get("offset", 0))
            if "zero" in f:
                body += struct.pack("<BBHIQ", arg, 0, off, 0, _int(f["zero"]))
            else:
                body += struct.pack("<BBHIQ", arg, f.get("size", 4), off, 0, _int(f.get("value", 0)) % 2**64)
        recs += _rec(REC_RULE, body + _cstr(r.get("lib", "")) + _cstr(r.get("function", r.get("nid", ""))))
    for mod in prof.get("modules", []):
        recs += _rec(REC_MODULE, _cstr(mod))
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, HEADER.size + len(recs), flags, prof["title"].encode(), fp,
                         _int(prof.get("save_blocks", 1024)), *app_params(prof, src),
                         _int(mem.get("runtime_reserve_mib", 64)), _int(mem.get("video_reserve_mib", 100)),
                         _int(mem.get("flex_mib", 0)), _int(mem.get("min_mib", 1024)), 0)
    blob = header + recs
    if len(blob) > MAX_BLOB:
        raise ProfileError(f"{prof['title']}: the compiled profile is {len(blob)} bytes (at most {MAX_BLOB})")
    return blob


# ---- command line ----

def starter(src: Path) -> dict:
    sfo = param_sfo(src)
    title = detect_title(src) or "CUSA00000"
    out = {"title": title, "name": sfo.get("TITLE", ""), "version": sfo.get("APP_VER", ""),
           "status": "untested"}
    fp = eboot_fingerprint(src)
    if fp:
        out["eboot"] = {"fingerprint": fp}
    out["pack"] = {"prefix": default_prefix(title)}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="ghostldr title profiles")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check", help="validate every profile")
    s = sub.add_parser("show", help="print a title's merged profile")
    s.add_argument("title")
    s.add_argument("--src", type=Path, help="the title's dump (for its param.sfo)")
    d = sub.add_parser("detect", help="identify a dump")
    d.add_argument("--src", type=Path, default=DEFAULT_SRC)
    n = sub.add_parser("new", help="print a starter profile for a dump")
    n.add_argument("--src", type=Path, default=DEFAULT_SRC)
    args = ap.parse_args()
    try:
        if args.cmd == "check":
            bad = 0
            for name in ["default"] + known_titles():
                try:
                    prof = load(None if name == "default" else name)
                    print(f"{name}: ok, {len(compile_blob(prof))} bytes compiled")
                except ProfileError as e:
                    print(e)
                    bad += 1
            return 1 if bad else 0
        if args.cmd == "show":
            title = normalize_title(args.title)
            if not title:
                raise ProfileError(f"{args.title!r} is not a title ID")
            prof = load(title)
            print(json.dumps({k: v for k, v in prof.items() if not k.startswith("_")}, indent=1))
            how = "own profile" if prof["_has_profile"] else "no profile: generic defaults"
            print(f"# {title}: {how}; app params {app_params(prof, args.src)}; "
                  f"{len(compile_blob(prof, args.src))} bytes compiled")
            return 0
        if args.cmd == "detect":
            sfo = param_sfo(args.src)
            print(f"title {detect_title(args.src) or '?'} (param.sfo TITLE_ID {sfo.get('TITLE_ID', '-')}, "
                  f"APP_VER {sfo.get('APP_VER', '-')}), eboot fingerprint {eboot_fingerprint(args.src) or '-'}")
            return 0
        print(json.dumps(starter(args.src), indent=1))
        return 0
    except ProfileError as e:
        raise SystemExit(str(e))


if __name__ == "__main__":
    raise SystemExit(main())
