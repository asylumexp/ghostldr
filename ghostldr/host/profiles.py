#!/usr/bin/env python3
"""Per-title compatibility profiles (ghostldr/data/titles/*.json).

Everything ghostldr does differently for one title lives in its profile, as
data: how its files are packed, its sceAppContent parameters, its save
container size, its memory plan (only for titles that need more than SWRR's
budget gives them), title-specific diagnostics, and HLE rules that answer
single imports without writing C.  default.json holds the generic values;
<TITLE>.json overrides them (objects merge key by key, everything else
replaces).  docs/compatibility.md explains every field.

A profile that names an eboot fingerprint is bound to that build: on any
other build of the title the core uses the generic defaults.  A memory plan
and the ghost_* features patch or read fixed offsets in the executable, so
they need the fingerprint.

run.py compiles the profiles into small blobs (boot_bundle) and sends them
with the core as a GHOSTLDR_PROFILE_HEX line, so changing a profile needs no
reinstall.  The blob format is struct prof_hdr and the records after it in
ghostldr/native/include/ghost.h; the core's side is native/src/profile.c.

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

TITLE_RE = re.compile(r"[A-Z0-9_-]{1,15}")    # 15: the 16-byte title fields keep their NUL
NID_RE = re.compile(r"[A-Za-z0-9+-]{11}")

# blob header flags (ghost.h PF_*)
PF_REQUESTED, PF_PLAN, PF_HOOKS, PF_DIAG, PF_TRACE_OPENS, PF_OWN = 1, 2, 4, 8, 16, 32
FEATURES = {
    "ghost_hooks": PF_HOOKS,        # hooks.c: inline diagnostic hooks at fixed eboot offsets
    "ghost_diagnostics": PF_DIAG,   # crash.c / hle.c: watchdog reads of globals at fixed eboot offsets
    "trace_opens": PF_TRACE_OPENS,  # log every file open (capture_order.py turns run.log into an order file)
}
# features whose C code was written against one title's executable
FEATURE_TITLE = {"ghost_hooks": "CUSA11456", "ghost_diagnostics": "CUSA11456"}
# run.py --gpu-heap-mib/--pool1-mib/--cpu-heap-mib -> the boot_ctx reserved[] slot
# the core reads them from (they override a plan table's mib; --slack-mib, slot
# 5, overrides memory.runtime_reserve_mib)
KNOBS = {"gpu_heap": 4, "pool1": 6, "cpu_heap": 7}
NO_KNOB = 0xFF
ROLES = {"set": 0, "absorb": 1}
ACTIONS = {"return": 1}

# blob layout, shared with ghost.h / profile.c
MAGIC = b"GLPF"
VERSION = 1
HEADER = struct.Struct("<4sHHII16s20sI5iIiII4x")  # struct prof_hdr, 96 bytes
REC = struct.Struct("<HHI")                        # struct prof_rec
REC_PLAN, REC_TABLE, REC_RULE, REC_TLS, REC_NAME, REC_EXEC, REC_NEO = 1, 2, 3, 4, 5, 6, 7
NAME_MAX = 127                 # bytes of UTF-8 in REC_NAME (the selector's name field is 128)
TLS_PER_REC = 15000            # offsets per REC_TLS record (its size is a u16)
MAX_BLOB = 0x10000
MAX_RULES = 32
MAX_BUNDLE = 64 << 10
TITLES_OUT = ROOT / "build/ghostfs"                # gfs_pack.py's packs (layout.json)


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


# fingerprints a toolchain gives every executable it builds: they name no build
GENERIC_FINGERPRINTS = {
    b"OPENORBIS-HOMEBREW\0\0".hex(): "OpenOrbis",   # create-fself, every version so far
}


def eboot_fingerprint(src: Path | None) -> str | None:
    """DT_SCE_FINGERPRINT of app0/eboot.bin (20 bytes, hex): names one build of
    a title whatever tool made the dump.  None for a plain ELF without one, and
    for a toolchain's generic one (homebrew_toolchain)."""
    fp = _raw_fingerprint(src)
    return None if fp in GENERIC_FINGERPRINTS else fp


def homebrew_toolchain(src: Path | None) -> str | None:
    """"OpenOrbis" for an eboot.bin built by the OpenOrbis toolchain (its
    generic fingerprint), else None."""
    return GENERIC_FINGERPRINTS.get(_raw_fingerprint(src) or "")


def eboot_module_name(src: Path | None) -> str | None:
    """The executable's module name (DT_SCE_MODULE_INFO): an OpenOrbis
    build's project name."""
    try:
        import orbis_elf as oe
        mod = oe.parse(Path(src) / "eboot.bin").module_info
        return mod.name if mod and mod.name else None
    except (OSError, ValueError, struct.error, ImportError, TypeError):
        return None


def _raw_fingerprint(src: Path | None) -> str | None:
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
    fingerprint matches (dumps without sce_sys/param.sfo), else for OpenOrbis
    homebrew its module name (homebrew_title)."""
    t = normalize_title(str(param_sfo(src).get("TITLE_ID", "")))
    if t:
        return t
    fp = eboot_fingerprint(src) if src else None
    if fp:
        for title in known_titles():
            if raw_profile(title).get("eboot", {}).get("fingerprint", "").lower() == fp:
                return title
    return homebrew_title(src)


def homebrew_title(src: Path | None) -> str | None:
    """A title ID for OpenOrbis homebrew without a usable param.sfo: its
    module name (the project's name), upper-cased, A-Z 0-9 _ - kept, at most
    15 characters."""
    if not src or not homebrew_toolchain(src):
        return None
    name = re.sub(r"[^A-Z0-9_-]", "", (eboot_module_name(src) or "").upper())[:15]
    return normalize_title(name)


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
            "features", "memory", "hle", "tls_sites", "exec"}
MAX_EXEC_RANGES = 6            # bxplan.h BX_MAX_RUNS
MEMORY_KEYS = {"mode", "runtime_reserve_mib", "video_reserve_mib", "flex_mib", "min_mib", "plan"}


def validate(p: dict) -> list[str]:
    e: list[str] = []
    if not _keys(e, "profile", p, TOP_KEYS):
        return e
    for k in ("name", "version", "status", "notes"):
        if k in p and not isinstance(p[k], str):
            e.append(f"{k} must be a string")
    if p.get("title") and not TITLE_RE.fullmatch(p["title"]):
        e.append(f"title {p['title']!r}: use A-Z 0-9 _ - (at most 15)")
    if "eboot" in p and _keys(e, "eboot", p["eboot"], {"fingerprint"}):
        fp = p["eboot"].get("fingerprint", "")
        if fp and not re.fullmatch(r"[0-9a-fA-F]{40}", fp):
            e.append("eboot.fingerprint must be 40 hex digits (profiles.py detect prints it)")
        elif fp and fp.lower() in GENERIC_FINGERPRINTS:
            e.append(f"eboot.fingerprint: every {GENERIC_FINGERPRINTS[fp.lower()]} build has this one, so it names "
                     "no build: leave it out")
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
            elif f in FEATURE_TITLE and not p.get("eboot", {}).get("fingerprint"):
                e.append(f"feature {f} reads fixed eboot offsets: set eboot.fingerprint")
    m = p.get("memory", {})
    if _keys(e, "memory", m, MEMORY_KEYS):
        if m.get("mode", "plan-free") not in ("plan-free", "plan"):
            e.append('memory.mode is "plan-free" or "plan"')
        for k in MEMORY_KEYS - {"mode", "plan"}:
            if k in m and _int(m[k], 0, 8192) is None:
                e.append(f"memory.{k} must be 0..8192 MiB")
        if m.get("mode") == "plan" and m.get("plan") and not m.get("runtime_reserve_mib"):
            e.append("memory.runtime_reserve_mib: a plan needs free memory left for ghostldr (its slack)")
        if m.get("mode") == "plan":
            _validate_plan(e, m.get("plan"))
            if not p.get("eboot", {}).get("fingerprint"):
                e.append("a memory plan patches fixed eboot offsets: set eboot.fingerprint")
        elif "plan" in m:
            e.append('memory.plan is only used with "mode": "plan"')
    tls = p.get("tls_sites", {})
    if not isinstance(tls, dict):
        e.append('tls_sites must map module file names to site lists ({"eboot.bin": "TITLE-tls-eboot.txt"})')
    else:
        for mod, f in tls.items():
            if not (isinstance(mod, str) and 0 < len(mod) < 32 and "/" not in mod):
                e.append(f"tls_sites: {mod!r} is not a module file name (at most 31 characters)")
            elif not (isinstance(f, str) and (TITLES / f).is_file()):
                e.append(f"tls_sites.{mod}: no file ghostldr/data/titles/{f} (tools/tls_sites.py --out writes it)")
        if tls and not p.get("eboot", {}).get("fingerprint"):
            e.append("tls_sites are text offsets of one build: set eboot.fingerprint")
    _validate_exec(e, p)
    rules = p.get("hle", [])
    if not isinstance(rules, list):
        e.append("hle must be a list of rules")
    else:
        if len(rules) > MAX_RULES:
            e.append(f"at most {MAX_RULES} hle rules")
        for i, r in enumerate(rules):
            _validate_rule(e, f"hle[{i}]", r)
    return e


def exec_ranges(p: dict) -> dict[str, list[tuple[int, int]]]:
    """The profile's "exec" code ranges per module file, as numbers."""
    return {mod: [(_int(a), _int(b)) for a, b in r] for mod, r in p.get("exec", {}).items()}


def _validate_exec(e: list, p: dict) -> None:
    """exec: {"eboot.bin": [["0x0", "0x2c00000"], ...]}: a module's code
    ranges (text offsets), used instead of what the core reads from the
    module when its code needs several JIT buffers (bigexec.c)."""
    ex = p.get("exec", {})
    if not isinstance(ex, dict):
        e.append('exec must map module file names to code ranges ({"eboot.bin": [["0x0", "0x2c00000"]]})')
        return
    for mod, ranges in ex.items():
        if not (isinstance(mod, str) and 0 < len(mod) < 32 and "/" not in mod):
            e.append(f"exec: {mod!r} is not a module file name (at most 31 characters)")
            continue
        ok = isinstance(ranges, list) and 0 < len(ranges) <= MAX_EXEC_RANGES and all(
            isinstance(r, list) and len(r) == 2 and _int(r[0], 0, 2**32 - 1) is not None
            and _int(r[1], 0, 2**32 - 1) is not None and _int(r[0]) < _int(r[1]) for r in ranges)
        if not ok:
            e.append(f"exec.{mod}: 1 to {MAX_EXEC_RANGES} [start, end] text offsets, start < end")
        elif any(_int(ranges[i][0]) < _int(ranges[i - 1][1]) for i in range(1, len(ranges))):
            e.append(f"exec.{mod}: ranges must be sorted and must not overlap")
    if ex and not p.get("eboot", {}).get("fingerprint"):
        e.append("exec ranges are text offsets of one build: set eboot.fingerprint")


def _validate_plan(e: list, plan) -> None:
    if not _keys(e, "memory.plan", plan, {"title_fixed_mib", "overcommit_mib", "lazy_sites", "tables", "neo"},
                 {"title_fixed_mib", "tables"}):
        return
    for k in ("title_fixed_mib", "overcommit_mib"):
        if k in plan and _int(plan[k], 0, 65535) is None:
            e.append(f"memory.plan.{k} must be 0..65535 MiB")
    neo = plan.get("neo")
    if neo is not None and _keys(e, "memory.plan.neo", neo,
                                 {"mode", "title_fixed_mib", "overcommit_mib", "knobs", "note"},
                                 {"mode", "title_fixed_mib"}):
        if _int(neo["mode"], 0, 15) is None:
            e.append("memory.plan.neo.mode must be the title's Pro memory mode, 0..15 (an index of its tables)")
        for k in ("title_fixed_mib", "overcommit_mib"):
            if k in neo and _int(neo[k], 0, 65535) is None:
                e.append(f"memory.plan.neo.{k} must be 0..65535 MiB")
        knobs = neo.get("knobs", {})
        if not isinstance(knobs, dict) or any(k not in KNOBS or _int(v, 1, 65535) is None for k, v in knobs.items()):
            e.append(f"memory.plan.neo.knobs maps a set table's knob ({', '.join(KNOBS)}) to its size in MiB")
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
        e.append(f"{where}.action: only return (answer the call with value, after the fills)")
    if "value" in r and _int(r["value"], -2**63, 2**64 - 1) is None:
        e.append(f"{where}.value must be an integer")
    if "log" in r and _int(r["log"], 0, 65535) is None:
        e.append(f"{where}.log must be 0..65535 (calls to log)")
    fills = r.get("fill", [])
    if not isinstance(fills, list) or len(fills) > 8:
        e.append(f"{where}.fill must list at most 8 writes")
        return
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
    payload += b"\0" * (-(len(payload) + REC.size) % 8)
    return REC.pack(kind, REC.size + len(payload), 0) + payload


def _cstr(s: str) -> bytes:
    return s.encode() + b"\0"


def utf8_cut(text: str, limit: int) -> str:
    """text, cut to at most `limit` bytes of UTF-8 without splitting a character."""
    raw = text.encode()
    return raw[:limit].decode("utf-8", "ignore") if len(raw) > limit else text


def tls_site_list(name: str) -> list[int]:
    """A tls_sites file (tools/tls_sites.py --out): hex text offsets, # comments."""
    out = []
    for line in (TITLES / name).read_text().splitlines():
        line = line.split("#")[0].strip()
        if line:
            out.append(int(line, 16))
    return sorted(out)


def compile_blob(prof: dict, src: Path | None = None, flags: int = 0, extra_features: tuple[str, ...] = (),
                 params: list[int] | None = None) -> bytes:
    """One title's blob (struct prof_hdr + records).  `params` are its app
    params when known already (layout.json), else they come from `src`."""
    import orbis_elf as oe
    if prof.get("_has_profile"):
        flags |= PF_OWN
    for f in list(prof.get("features", [])) + list(extra_features):
        flags |= FEATURES[f]
    fp = bytes.fromhex(prof.get("eboot", {}).get("fingerprint", "") or "00" * 20)
    mem = prof.get("memory", {})
    recs = b""
    if mem.get("mode") == "plan":
        flags |= PF_PLAN
        plan = mem["plan"]
        sites = [_int(x) for x in plan.get("lazy_sites", [])]
        recs += _rec(REC_PLAN, struct.pack(f"<3I{len(sites)}I", _int(plan["title_fixed_mib"]),
                                           _int(plan.get("overcommit_mib", 0)), len(sites), *sites))
        for t in plan["tables"]:
            exp = [_int(v) for v in t.get("expect", [])]
            recs += _rec(REC_TABLE, struct.pack("<IBBHII", _int(t["table"]), ROLES[t["role"]],
                                                KNOBS.get(t.get("knob"), NO_KNOB), len(exp), _int(t.get("mib", 0)),
                                                _int(t.get("min_mib", 0)))
                         + struct.pack(f"<{len(exp)}Q", *exp) + _cstr(t.get("what", "table")[:40]))
        if plan.get("neo"):     # a Pro-mode host (Jak X) runs the title in this memory mode
            neo = plan["neo"]
            sizes = [0] * 8
            for k, v in neo.get("knobs", {}).items():
                sizes[KNOBS[k]] = _int(v)
            recs += _rec(REC_NEO, struct.pack("<3I8I", _int(neo["mode"]), _int(neo["title_fixed_mib"]),
                                              _int(neo.get("overcommit_mib", 0)), *sizes))
    for r in prof.get("hle", []):
        nid = r.get("nid") or oe.compute_nid(r["function"])
        fills = r.get("fill", [])
        body = struct.pack("<BBH12sQ", ACTIONS[r.get("action", "return")], len(fills), _int(r.get("log", 1)),
                           nid.encode(), _int(r.get("value", 0)) % 2**64)
        for f in fills:
            arg, off = _int(f["arg"]) - 1, _int(f.get("offset", 0))
            if "zero" in f:
                body += struct.pack("<BBHIQ", arg, 0, off, 0, _int(f["zero"]))
            else:
                body += struct.pack("<BBHIQ", arg, f.get("size", 4), off, 0, _int(f.get("value", 0)) % 2**64)
        recs += _rec(REC_RULE, body + _cstr(r.get("lib", "")) + _cstr(r.get("function", r.get("nid", ""))))
    for mod, f in sorted(prof.get("tls_sites", {}).items()):
        offs = tls_site_list(f)
        for i in range(0, max(len(offs), 1), TLS_PER_REC):
            chunk = offs[i:i + TLS_PER_REC]
            recs += _rec(REC_TLS, struct.pack("<32sI", mod.encode(), len(chunk)) + struct.pack(f"<{len(chunk)}I", *chunk))
    if prof.get("name"):           # the game selector shows it when the title has no param.sfo
        recs += _rec(REC_NAME, _cstr(utf8_cut(prof["name"], NAME_MAX)))
    for mod, ranges in sorted(exec_ranges(prof).items()):
        flat = [v for r in ranges for v in r]
        recs += _rec(REC_EXEC, struct.pack("<32sI", mod.encode(), len(ranges)) + struct.pack(f"<{len(flat)}I", *flat))
    video = mem.get("video_reserve_mib")
    params = params if params is not None else app_params(prof, src)
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, HEADER.size + len(recs), flags, prof["title"].encode(), fp,
                         _int(prof.get("save_blocks", 1024)), *params,
                         _int(mem.get("runtime_reserve_mib", 64)), -1 if video is None else _int(video),
                         _int(mem.get("flex_mib", 0)), _int(mem.get("min_mib", 1024)))
    blob = header + recs
    if len(blob) > MAX_BLOB:
        raise ProfileError(f"{prof['title']}: the compiled profile is {len(blob)} bytes (at most {MAX_BLOB})")
    return blob


def read_blobs(data: bytes) -> list[dict]:
    """The blobs of a bundle, decoded (as profile.c walks them)."""
    out, pos = [], 0
    while pos + HEADER.size <= len(data) and data[pos:pos + 4] == MAGIC:
        (_, version, hsize, total, flags, title, fp, save_blocks, *rest) = HEADER.unpack_from(data, pos)
        params, (reserve, video, flex, min_mib) = rest[:5], rest[5:]
        recs, rp = [], pos + hsize
        while rp + REC.size <= pos + total:
            kind, size, _ = REC.unpack_from(data, rp)
            recs.append((kind, data[rp + REC.size:rp + size]))
            rp += size
        out.append({"title": title.rstrip(b"\0").decode(), "flags": flags, "fingerprint": fp.hex(),
                    "save_blocks": save_blocks, "app_params": list(params), "runtime_reserve_mib": reserve,
                    "video_reserve_mib": video, "flex_mib": flex, "min_mib": min_mib, "records": recs,
                    "size": total})
        pos += total
    return out


def packed(title: str) -> dict:
    """What gfs_pack.py recorded for the title (build/ghostfs/<TITLE>/layout.json), or {}."""
    try:
        return json.loads((TITLES_OUT / title / "layout.json").read_text())
    except (OSError, ValueError):
        return {}


def boot_bundle(title: str | None = None, extra_features: tuple[str, ...] = ()) -> bytes:
    """What run.py sends as GHOSTLDR_PROFILE_HEX: with a title, its blob
    (marked requested) and the generic default; without, one blob per title
    this computer knows (profiles and packs: the core picks the installed
    one, or the one whose eboot fingerprint matches) and the default.  Ends
    with 8 zero bytes."""
    if title:
        t = normalize_title(title)
        if not t:
            raise ProfileError(f"{title!r} is not a title ID")
        titles, flags = [t], PF_REQUESTED
    else:
        packs = [f.parent.name for f in TITLES_OUT.glob("*/layout.json")]
        titles, flags = sorted(set(known_titles()) | {t for t in packs if normalize_title(t)}), 0
    blobs = []
    for t in titles:
        info = packed(t)
        src = Path(info["src"]) if info.get("src") else (DEFAULT_SRC if detect_title(DEFAULT_SRC) == t else None)
        params = info.get("app_params") if not (src and src.is_dir()) else None
        blobs.append(compile_blob(load(t), src if src and src.is_dir() else None, flags, extra_features, params))
    blobs.append(compile_blob(load(None), None, 0, extra_features))
    data = b"".join(blobs) + b"\0" * 8
    if len(data) > MAX_BUNDLE:
        raise ProfileError(f"{len(titles)} profiles take {len(data)} bytes (at most {MAX_BUNDLE}): pass --title")
    return data


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
            src = args.src or (Path(packed(title)["src"]) if packed(title).get("src") else None)
            print(f"# {title}: {how}; app params {app_params(prof, src)}; "
                  f"{len(compile_blob(prof, src))} bytes compiled")
            return 0
        if args.cmd == "detect":
            sfo = param_sfo(args.src)
            kit = homebrew_toolchain(args.src)
            fp = f"{kit}'s generic one" if kit else eboot_fingerprint(args.src) or "-"
            print(f"title {detect_title(args.src) or '?'} (param.sfo TITLE_ID {sfo.get('TITLE_ID', '-')}, "
                  f"APP_VER {sfo.get('APP_VER', '-')}), eboot fingerprint {fp}"
                  + (f", module {eboot_module_name(args.src)}" if kit else ""))
            return 0
        print(json.dumps(starter(args.src), indent=1))
        return 0
    except ProfileError as e:
        raise SystemExit(str(e))


if __name__ == "__main__":
    raise SystemExit(main())
