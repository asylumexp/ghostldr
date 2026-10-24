#!/usr/bin/env python3
"""Import coverage of a PS4 title under ghostldr (offline).

Lists every import of a title's eboot.bin and sce_module/*.prx and says how
ghostldr's resolver (native/src/resolve.c) would bind it, in its order:

  CORE-HLE    a core HLE table entry (struct hle_ent tables in native/src)
  EXT-HLE     a runtime-extension HLE entry (native/ext/, or --ext paths)
  TITLE       exported by one of the title's own modules
  HOST        a system module a PS4 app can load (the sysmodule list), bound
              by name with dlsym
  UNRESOLVED  no name for the NID, or nothing provides it (the loader would
              route it to a logging stub that returns 0)

Inputs: the NID name lists (build/nid/known_names.txt), the HLE tables, the
system module list (ghostldr/data/ps5_ps4_sysmodules.txt; only
/system/common/lib is visible to a PS4 app), and the sce_module files the boot
loads (gen_nids.boot_modules; a BUNDLED[] list in boot.c if one exists). HOST
only means the module file exists; whether it exports the name isn't checked.

Extension HLE entries are read from C files ({"libX", "name", fn},
MACRO("libX", name), or {"libX", NULL, fn, "NID"} for a function with no
known name, matched by its NID; anywhere in the file) or from text lists
("libX name" per line), under native/ext/ or the --ext paths.

It also says whether the title's code fits the executable memory ghostldr
has inside SWRR (tools/exec_fit.py: the EE buffer, several buffers, or not at
all), and counts, per library, how many of the imported functions shadPS4
registers and whether its version is a real implementation or a stub. shadPS4
is only read for data (its LIB_FUNCTION lines and function bodies).

    python3 ghostldr/tools/import_report.py build/ghost/app0 \\
        --shadps4 build/ref/shadPS4 --md out.md --json out.json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import exec_fit  # noqa: E402
import gen_nids  # noqa: E402
import orbis_elf as oe  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ("CORE-HLE", "EXT-HLE", "TITLE", "HOST", "UNRESOLVED")
SEVERITY = {"CORE-HLE": 0, "EXT-HLE": 0, "TITLE": 0, "HOST": 1, "UNRESOLVED": 2}
# a PS4 app's sandbox mounts only this system library directory
APP_LIB_DIRS = ("/system/common/lib",)
# module names whose code lives in a file of another name; resolve.c reaches
# these through the importing file's DT_NEEDED, this table only adds notes
MODULE_FILE_ALIASES = {
    "libSceAppContentUtil": ("libSceAppContent",),
    "libSceNpScore": ("libSceNpScoreRanking",),
    "libSceSsl": ("libSceSsl2",),
    "libSceGnmDriver": ("libSceGnmDriverForNeoMode", "libSceGnmDriverCompat1"),
}
SYM_TYPES = {oe.STT_NOTYPE: "notype", oe.STT_OBJECT: "object", oe.STT_FUNC: "func", oe.STT_TLS: "tls"}
STATUS_RANK = {"impl": 3, "partial": 2, "stub": 1, "unknown": 0}


# ---------------------------------------------------------------- C sources

_SPECIAL = re.compile(r"[/\"']")


def strip_comments(text: str) -> str:
    """Drop C/C++ comments, keeping string literals and line numbers."""

    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        m = _SPECIAL.search(text, i)
        j = m.start() if m else n
        out.append(text[i:j])
        i = j
        if i >= n:
            break
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            k = text.find("\n", i)
            i = n if k < 0 else k
        elif c == "/" and nxt == "*":
            k = text.find("*/", i + 2)
            end = n if k < 0 else k + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif c in "\"'":
            k = i + 1
            while k < n and text[k] != c and text[k] != "\n":
                k += 2 if text[k] == "\\" else 1
            out.append(text[i:k + 1])
            i = k + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


_DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+"([^"\n]*)"[ \t]*$', re.M)
_HLE_TABLE = re.compile(r"struct\s+hle_ent\s+\w+\s*\[\s*\w*\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;", re.S)
# {"libX", "name", fn} or {LK, "name", fn} (LK a #define of a library name)
_ENTRY = re.compile(r'\{\s*("lib\w*"|[A-Za-z_]\w*)\s*,\s*"(\w+)"\s*,')
# E("libX", name): a macro that stringifies the function name
_MACRO_ENTRY = re.compile(r'\b[A-Z_][A-Z0-9_]*\(\s*("lib\w*"|[A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)')
# {"libX", NULL, fn, "NID"}: a function with no known name, matched by its NID
_NID_ENTRY = re.compile(r'\{\s*("lib\w*"|[A-Za-z_]\w*)\s*,\s*NULL\s*,\s*\w+\s*,\s*"([A-Za-z0-9+\-]{11})"\s*\}')
NID_KEY = "nid:"  # table keys (library, NID_KEY + nid) for entries matched by NID


def _lib_value(token: str, defines: dict[str, str]) -> str | None:
    value = token[1:-1] if token.startswith('"') else defines.get(token)
    return value if value and re.fullmatch(r"lib\w*", value) else None


def hle_entries(text: str, tables_only: bool) -> list[tuple[str, str]]:
    """(library, function) pairs registered in a C file's HLE tables; an entry
    matched by NID alone gives (library, NID_KEY + nid)."""

    text = strip_comments(text)
    defines = dict(_DEFINE.findall(text))
    bodies = _HLE_TABLE.findall(text) if tables_only else [text]
    out = []
    for body in bodies:
        for regex, prefix in ((_ENTRY, ""), (_MACRO_ENTRY, ""), (_NID_ENTRY, NID_KEY)):
            for token, name in regex.findall(body):
                lib = _lib_value(token, defines)
                if lib:
                    out.append((lib, prefix + name))
    return out


# budget.c's memory calls ({"name", fn, every} in k_mem[]): resolved by NID
# ahead of everything else when the title has no memory plan (the default),
# and those marked every for planned titles too
_PLAN_FREE_TABLE = re.compile(r"\bk_mem\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;", re.S)
_PLAN_FREE_ENTRY = re.compile(r'\{\s*"(\w+)"\s*,\s*\w+\s*(?:,\s*\w+\s*)?\}')


def load_core_hle(native: Path) -> dict[tuple[str, str], str]:
    """Core HLE entries: every struct hle_ent table in native/src/*.c, and
    budget.c's plan-free memory calls (libkernel)."""

    table: dict[tuple[str, str], str] = {}
    for path in sorted((native / "src").glob("*.c")):
        text = path.read_text(errors="replace")
        for key in hle_entries(text, tables_only=True):
            table.setdefault(key, path.name)
        for body in _PLAN_FREE_TABLE.findall(strip_comments(text)):
            for name in _PLAN_FREE_ENTRY.findall(body):
                table.setdefault(("libkernel", name), f"{path.name} (plan-free)")
    return table


def load_module_files(native: Path) -> dict[str, str]:
    """resolve.c's k_module_files: modules a sysmodule file of another name
    provides (libSceFreeType in libSceFreeTypeOl.sprx)."""

    try:
        text = strip_comments((native / "src/resolve.c").read_text(errors="replace"))
    except OSError:
        return {}
    m = re.search(r"k_module_files\s*\[\s*\]\s*\[\s*2\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    return dict(re.findall(r'\{\s*"(\w+)"\s*,\s*"(\w+)"\s*\}', m.group(1))) if m else {}


def load_ext_hle(paths: list[Path]) -> dict[tuple[str, str], str]:
    """Extension HLE entries from C sources (table entries anywhere in the
    file) or plain lists ("library function" per line, '#' comments)."""

    table: dict[tuple[str, str], str] = {}
    files: list[Path] = []
    for p in paths:
        if p.is_dir():
            files += sorted(f for f in p.rglob("*") if f.is_file())
        elif p.is_file():
            files.append(p)
    for f in files:
        if f.suffix in (".c", ".h", ".inc", ".cc", ".cpp"):
            entries = hle_entries(f.read_text(errors="replace"), tables_only=False)
        elif f.suffix in (".txt", ".tsv", ".list"):
            entries = []
            for line in f.read_text(errors="replace").splitlines():
                parts = re.split(r"[\s:,]+", line.split("#", 1)[0].strip())
                if len(parts) >= 2 and parts[0].startswith("lib"):
                    entries.append((parts[0], parts[1]))
        else:
            continue
        for key in entries:
            table.setdefault(key, str(f))
    return table


def load_bundled(native: Path) -> list[str] | None:
    """The sce_module files boot.c loads (BUNDLED[]), or None if not found."""

    boot = native / "src" / "boot.c"
    if not boot.is_file():
        return None
    m = re.search(r"BUNDLED\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", strip_comments(boot.read_text()), re.S)
    return re.findall(r'"([^"]+)"', m.group(1)) if m else None


# ---------------------------------------------------------------- sysmodules

@dataclass
class SysModule:
    name: str
    size: int
    directory: str


def load_sysmodules(path: Path) -> dict[str, list[SysModule]]:
    """name -> entries, from 'name<TAB>size<TAB>directory' lines."""

    mods: dict[str, list[SysModule]] = defaultdict(list)
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 3:
            mods[parts[0]].append(SysModule(parts[0], int(parts[1]), parts[2]))
    return dict(mods)


def app_visible(sysmods: dict[str, list[SysModule]], file: str) -> bool:
    return any(m.directory in APP_LIB_DIRS for m in sysmods.get(file, ()))


def shares_prefix(base: str, modname: str) -> bool:
    """resolve.c's DT_NEEDED match: one name starts with the other, 8+ chars."""

    n = min(len(base), len(modname))
    return n >= 8 and base[:n] == modname[:n] and base != modname


MODULE_FILES = load_module_files(ROOT / "ghostldr/native")


def host_file(modname: str | None, needed: list[str], sysmods: dict[str, list[SysModule]]) -> tuple[str | None, str]:
    """The system file resolve.c would load for a module, and how it got it."""

    if not modname:
        return None, "no module"
    if modname == "libkernel":
        return "libkernel.sprx", "SWRR's libkernel"
    if app_visible(sysmods, modname + ".sprx"):
        return modname + ".sprx", "module name"
    # host_module_handle_for: DT_NEEDED entries sharing a prefix of 8+ chars
    for dep in needed:
        base = dep[:-4] if dep.endswith(".prx") else dep
        if shares_prefix(base, modname) and app_visible(sysmods, base + ".sprx"):
            return base + ".sprx", f"DT_NEEDED {dep}"
    alias = MODULE_FILES.get(modname)
    if alias and app_visible(sysmods, alias + ".sprx"):
        return alias + ".sprx", "resolve.c's module files"
    return None, "missing"


# ---------------------------------------------------------------- shadPS4

_LIB_FUNCTION = re.compile(
    r'\bLIB_(FUNCTION|OBJ)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\d+\s*,\s*"([^"]+)"\s*,\s*(.+?)\)\s*;', re.S)
_STUB_MARK = re.compile(r'"[^"\n]*\b(?:STUBBED|DUMMY)\b[^"\n]*"|\bUNREACHABLE')
_PARTIAL_MARK = re.compile(r'"[^"\n]*\bPARTIAL\b[^"\n]*"')
_LOG_STMT = re.compile(r"\b(?:LOG_\w+|UNREACHABLE\w*)\s*\(")


@dataclass
class ShadEntry:
    nid: str
    lib: str
    module: str
    function: str
    file: str
    status: str  # impl / partial / stub / unknown


def _match_close(text: str, i: int, open_c: str, close_c: str) -> int:
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            k = i + 1
            while k < n and text[k] != c and text[k] != "\n":
                k += 2 if text[k] == "\\" else 1
            i = k + 1
            continue
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


_QUALIFIERS = re.compile(r"\s*(?:(?:const|noexcept|override|final)\b\s*)*(?:->\s*[\w:<>\s\*&]+?)?\s*(\{|;|:)")


def find_function_body(text: str, name: str) -> str | None:
    """Body (with braces) of a function definition named name, if any."""

    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        prefix = text[line_start:m.start()]
        if re.search(r"(?:\breturn|[=(,!?.]|->|&&|\|\|)\s*$", prefix):
            continue
        close = _match_close(text, m.end() - 1, "(", ")")
        if close < 0:
            continue
        q = _QUALIFIERS.match(text, close + 1)
        if not q or q.group(1) != "{":
            continue
        open_brace = q.end() - 1
        end = _match_close(text, open_brace, "{", "}")
        if end > 0:
            return text[open_brace:end + 1]
    return None


def classify_body(body: str | None) -> str:
    """impl / partial / stub from a shadPS4 function body (None: not found)."""

    if body is None:
        return "unknown"
    inner = body[1:-1]
    if _PARTIAL_MARK.search(inner):
        return "partial"
    stubbed = _STUB_MARK.search(inner)
    if not stubbed:
        return "impl"
    # a stub logs and returns, maybe zeroing an output or checking a pointer:
    # count the other statements
    rest = 0
    for stmt in inner.split(";"):
        s = re.sub(r"^(?:[{}\s]|else\b|if\s*\(.*?\)\s*\{?)+", "", stmt, flags=re.S).strip()
        if not s or _LOG_STMT.match(s) or re.match(r"(?:return|break)\b", s):
            continue
        rest += 1
    if rest <= 2:
        return "stub"
    # a long body that only has UNREACHABLE on an impossible path is real code
    return "partial" if re.search(r"\b(?:STUBBED|DUMMY)\b", inner) else "impl"


_DEF_START = re.compile(r"(?m)^(?:[A-Za-z_][\w:<>,]*[ \t*&]+)+((?:\w+::)*\w+)[ \t]*\(")


def index_definitions(text: str) -> dict[str, str]:
    """name -> body of the functions defined at column 0 (shadPS4 doesn't
    indent namespaces, so that covers nearly all of them)."""

    defs: dict[str, str] = {}
    for m in _DEF_START.finditer(text):
        name = m.group(1).split("::")[-1]
        if name in defs:
            continue
        close = _match_close(text, m.end() - 1, "(", ")")
        if close < 0:
            continue
        q = _QUALIFIERS.match(text, close + 1)
        if not q or q.group(1) != "{":
            continue
        end = _match_close(text, q.end() - 1, "{", "}")
        if end > 0:
            defs[name] = text[q.end() - 1:end + 1]
    return defs


class ShadPS4:
    """LIB_FUNCTION registrations of a shadPS4 source tree."""

    def __init__(self, root: Path):
        self.root = root
        libs = root / "src" / "core" / "libraries"
        if not libs.is_dir():
            raise SystemExit(f"{root}: no src/core/libraries")
        self.texts: dict[Path, str] = {}
        self.by_top: dict[str, list[Path]] = defaultdict(list)
        for f in sorted(libs.rglob("*")):
            if f.suffix in (".cpp", ".h", ".inc", ".hpp"):
                self.texts[f] = strip_comments(f.read_text(errors="replace"))
                self.by_top[f.relative_to(libs).parts[0]].append(f)
        self.libs = libs
        self._defs: dict[Path, dict[str, str]] = {}
        self._extra: list[Path] | None = None
        self.by_key: dict[tuple[str, str], ShadEntry] = {}
        self.by_nid: dict[str, list[ShadEntry]] = defaultdict(list)
        self.commit = self._commit()
        for f, text in self.texts.items():
            for kind, nid, lib, module, fn in _LIB_FUNCTION.findall(text):
                fn = re.sub(r"\s+", "", fn)
                m = re.fullmatch(r"ORBIS\((.+)\)", fn)
                fn = (m.group(1) if m else fn).split("::")[-1].lstrip("&")
                status = "impl" if kind == "OBJ" else classify_body(self._body(f, fn))
                entry = ShadEntry(nid, lib, module, fn, str(f.relative_to(root)), status)
                old = self.by_key.get((lib, nid))
                if old is None or STATUS_RANK[status] > STATUS_RANK[old.status]:
                    self.by_key[(lib, nid)] = entry
                self.by_nid[nid].append(entry)

    def _commit(self) -> str | None:
        try:
            out = subprocess.run(["git", "-C", str(self.root), "rev-parse", "--short", "HEAD"],
                                 capture_output=True, text=True, timeout=10)
            return out.stdout.strip() or None
        except (OSError, subprocess.SubprocessError):
            return None

    def _index(self, f: Path) -> dict[str, str]:
        if f not in self._defs:
            text = self.texts.get(f)
            if text is None:
                text = strip_comments(f.read_text(errors="replace"))
            self._defs[f] = index_definitions(text)
        return self._defs[f]

    def _body(self, reg_file: Path, fn: str) -> str | None:
        """The registered function's body: the registering file first, then
        its library directory, then all libraries, then the rest of src/."""

        near = [reg_file] + [f for f in self.by_top[reg_file.relative_to(self.libs).parts[0]] if f != reg_file]
        for f in near:
            if fn in self._index(f):
                return self._index(f)[fn]
        for f in near:  # indented definitions
            body = find_function_body(self.texts[f], fn)
            if body is not None:
                return body
        for f in self.texts:
            if fn in self._index(f):
                return self._index(f)[fn]
        if self._extra is None:
            self._extra = [f for f in sorted((self.root / "src").rglob("*"))
                           if f.suffix in (".cpp", ".h") and f not in self.texts]
        for f in self._extra:
            if fn in self._index(f):
                return self._index(f)[fn]
        return None

    def lookup(self, lib: str | None, nid: str) -> tuple[ShadEntry | None, bool]:
        """(entry, exact library match)"""

        if lib and (lib, nid) in self.by_key:
            return self.by_key[(lib, nid)], True
        cands = self.by_nid.get(nid)
        if cands:
            return max(cands, key=lambda e: STATUS_RANK[e.status]), False
        return None, False


# ---------------------------------------------------------------- the title

@dataclass
class Import:
    file: str
    library: str | None
    module: str | None
    nid: str
    name: str | None
    type: str
    weak: bool
    cls: str = ""
    provider: str | None = None  # HLE source file, title module, or system file
    reason: str = ""
    host_file: str | None = None  # the system file resolve.c would try for the module
    shadps4: str | None = None  # impl / partial / stub, None if not registered
    shadps4_function: str | None = None
    shadps4_lib_match: bool = True


@dataclass
class TitleModule:
    file: str
    module: str | None
    needed: list[str]
    export_libs: list[str]
    exports: set[str] = field(default_factory=set)
    imports: int = 0
    loaded_by_boot: bool | None = None  # None: the executable, or boot.c not found


def title_files(app0: Path) -> list[Path]:
    files = [app0 / "eboot.bin"] if (app0 / "eboot.bin").is_file() else []
    mod_dir = app0 / "sce_module"
    if mod_dir.is_dir():
        files += sorted(f for f in mod_dir.iterdir() if f.suffix in (".prx", ".sprx"))
    if files and files[0].name == "eboot.bin":   # modules it needs that live elsewhere (Unity: Media/Modules)
        files += [f for f in gen_nids.module_candidates(app0) if f not in files]
    if not files:
        raise SystemExit(f"{app0}: no eboot.bin or sce_module/*.prx")
    return files


def build_report(app0: Path, names: dict[str, str], core: dict, ext: dict,
                 sysmods: dict[str, list[SysModule]], shad: ShadPS4 | None,
                 bundled: list[str] | None = None) -> dict:
    images = [(f, oe.parse(f)) for f in title_files(app0)]
    modules: list[TitleModule] = []
    for f, img in images:
        tm = TitleModule(f.name if f.parent == app0 else f"{f.parent.name}/{f.name}",
                         img.module_info.name if img.module_info else None, img.needed,
                         sorted({lib.name for lib in img.export_libs.values()}))
        tm.exports = {s.nid for s in img.exports()}
        if bundled is not None and f.name != "eboot.bin":
            tm.loaded_by_boot = f.name in bundled
        modules.append(tm)

    imports: list[Import] = []
    for (f, img), tm in zip(images, modules):
        for sym, lib, mod in img.imports():
            imp = Import(tm.file, lib.name if lib else None, mod.name if mod else None, sym.nid,
                         names.get(sym.nid), SYM_TYPES.get(sym.type, str(sym.type)), sym.bind == oe.STB_WEAK)
            classify(imp, img.needed, core, ext, modules, sysmods)
            if shad:
                entry, exact = shad.lookup(imp.library, imp.nid)
                if entry:
                    imp.shadps4, imp.shadps4_function, imp.shadps4_lib_match = entry.status, entry.function, exact
            imports.append(imp)
            tm.imports += 1

    return {
        "app0": str(app0),
        "modules": [dict(asdict(m), exports=len(m.exports)) for m in modules],
        "inputs": {
            "names": len(names),
            "core_hle_entries": len(core),
            "ext_hle_entries": len(ext),
            "system_modules_visible": sum(1 for v in sysmods.values() if any(m.directory in APP_LIB_DIRS for m in v)),
            "shadps4": None if shad is None else {"path": str(shad.root), "commit": shad.commit,
                                                  "registrations": len(shad.by_key)},
            "bundled": bundled,
        },
        "summary": summarize(imports),
        "libraries": summarize_libraries(imports),
        "module_files": module_files(imports, modules, sysmods),
        "linked_files": linked_files(modules, sysmods),
        "imports": [asdict(i) for i in imports],
    }


def classify(imp: Import, needed: list[str], core: dict, ext: dict,
             modules: list[TitleModule], sysmods: dict[str, list[SysModule]]) -> None:
    """Bind one import the way resolve.c's resolve_import does."""

    title_mods = {tm.module for tm in modules}
    if imp.module not in title_mods:
        imp.host_file = host_file(imp.module, needed, sysmods)[0]

    def hle(table: dict) -> str | None:
        if imp.library and (src := table.get((imp.library, NID_KEY + imp.nid))):
            return src
        if not imp.name:
            return None
        if imp.library:
            return table.get((imp.library, imp.name))
        return next((src for (lib, name), src in table.items() if name == imp.name), None)

    if (src := hle(core)):
        imp.cls, imp.provider = "CORE-HLE", src
        return
    if (src := hle(ext)):
        imp.cls, imp.provider = "EXT-HLE", src
        return
    for tm in modules:
        if imp.library in tm.export_libs and imp.nid in tm.exports:
            imp.cls, imp.provider = "TITLE", tm.file
            if tm.loaded_by_boot is False:
                imp.reason = f"{tm.file} isn't loaded at boot (no import needs it)"
            return
    if not imp.library:
        imp.cls, imp.reason = "UNRESOLVED", "unknown library id"
        return
    if not imp.name:
        imp.cls, imp.reason = "UNRESOLVED", "no name for this NID (host lookup is by name)"
        return
    if imp.host_file:
        imp.cls, imp.provider = "HOST", imp.host_file
        return
    imp.cls = "UNRESOLVED"
    imp.reason = f"no system module file for {imp.module}"
    aliases = [a + ".sprx" for a in MODULE_FILE_ALIASES.get(imp.module or "", ()) if app_visible(sysmods, a + ".sprx")]
    if aliases:
        imp.reason += f" ({', '.join(aliases)} exists but the importing file doesn't name it)"
    elif any(m.directory not in APP_LIB_DIRS for m in sysmods.get(f"{imp.module}.sprx", ())):
        dirs = sorted({m.directory for m in sysmods[f"{imp.module}.sprx"]})
        imp.reason += f" (only in {', '.join(dirs)}, which a PS4 app can't see)"


def module_files(imports: list[Import], modules: list[TitleModule], sysmods: dict[str, list[SysModule]]) -> list[dict]:
    """Imported system modules whose file isn't simply <module>.sprx: the
    module name, the DT_NEEDED file the title links against, and the file
    resolve.c loads."""

    needed_of = {tm.file: tm.needed for tm in modules}
    title_mods = {tm.module for tm in modules}
    rows: dict[str, dict] = {}
    for imp in imports:
        mod = imp.module
        if not mod or mod in title_mods or mod == "libkernel":
            continue
        bases = [dep[:-4] if dep.endswith(".prx") else dep for dep in needed_of[imp.file]]
        if mod in bases:
            if imp.host_file == f"{mod}.sprx":
                continue
            linked = [mod]
        else:  # resolve.c's DT_NEEDED fallback: a shared prefix of 8+ characters
            linked = [b for b in bases if shares_prefix(b, mod)]
        row = rows.setdefault(mod, {"module": mod, "linked": set(), "loads": imp.host_file, "classes": Counter()})
        row["linked"].update(linked)
        row["classes"][imp.cls] += 1
    out = []
    for row in rows.values():
        mod = row["module"]
        linked = sorted(row["linked"])
        notes = [f"{b}.sprx isn't on the console" for b in linked if b != mod and not app_visible(sysmods, b + ".sprx")]
        if row["loads"] is None:
            notes.append("no file found")
        elif row["loads"] != f"{mod}.sprx":
            notes.append(f"no {mod}.sprx; found through DT_NEEDED")
        out.append({"module": mod, "linked": [b + ".prx" for b in linked], "loads": row["loads"],
                    "sites_by_class": dict(row["classes"]), "notes": notes})
    return sorted(out, key=lambda r: r["module"])


def linked_files(modules: list[TitleModule], sysmods: dict[str, list[SysModule]]) -> list[dict]:
    """Every DT_NEEDED file outside the title's sce_module, and whether the
    console has it where a PS4 app can load it."""

    own = {Path(tm.file).name for tm in modules}
    rows: dict[str, dict] = {}
    for tm in modules:
        for dep in tm.needed:
            if dep in own:
                continue
            base = dep[:-4] if dep.endswith(".prx") else dep
            row = rows.setdefault(dep, {"linked": dep, "file": base + ".sprx", "by": [],
                                        "present": app_visible(sysmods, base + ".sprx"),
                                        "elsewhere": sorted({m.directory for m in sysmods.get(base + ".sprx", ())
                                                             if m.directory not in APP_LIB_DIRS})})
            row["by"].append(tm.file)
    return sorted(rows.values(), key=lambda r: (r["present"], r["linked"].lower()))


def _distinct(imports: list) -> dict:
    """One import per (library, NID): the most severe class among its sites.
    Works on Import objects and on their dicts."""

    get = (lambda i, k: i[k]) if imports and isinstance(imports[0], dict) else getattr
    out: dict = {}
    for imp in imports:
        key = (get(imp, "library"), get(imp, "nid"))
        if key not in out or SEVERITY[get(imp, "cls")] > SEVERITY[get(out[key], "cls")]:
            out[key] = imp
    return out


def summarize(imports: list[Import]) -> dict:
    distinct = _distinct(imports)
    return {
        "import_sites": len(imports),
        "functions": len(distinct),
        "sites_by_class": {c: sum(1 for i in imports if i.cls == c) for c in CLASSES},
        "functions_by_class": {c: sum(1 for i in distinct.values() if i.cls == c) for c in CLASSES},
        "unnamed": sum(1 for i in distinct.values() if not i.name),
    }


def risk(row: dict) -> str:
    if row["classes"]["UNRESOLVED"]:
        return "HIGH"
    if row["classes"]["HOST"]:
        return "MEDIUM"
    return "LOW"


def summarize_libraries(imports: list[Import]) -> list[dict]:
    by_lib: dict[str | None, list[Import]] = defaultdict(list)
    for imp in _distinct(imports).values():
        by_lib[imp.library].append(imp)
    sites = Counter(i.library for i in imports)
    rows = []
    for lib, imps in by_lib.items():
        classes = {c: sum(1 for i in imps if i.cls == c) for c in CLASSES}
        shad = Counter(i.shadps4 or "none" for i in imps)
        modules = sorted({i.module or "?" for i in imps})
        title_side = all(i.cls == "TITLE" for i in imps)
        row = {
            "library": lib,
            "modules": modules,
            "importers": sorted({i.file for i in imports if i.library == lib}),
            "functions": len(imps),
            "sites": sites[lib],
            "classes": classes,
            # the system files resolve.c would load for the library's module(s)
            "host_files": sorted({i.host_file for i in imps if i.host_file}),
            "host_missing": not title_side and any(i.host_file is None and i.module not in (None, "libkernel")
                                                   and i.cls != "TITLE" for i in imps),
            "shadps4": {k: shad.get(k, 0) for k in ("impl", "partial", "stub", "unknown", "none")},
        }
        row["risk"] = risk(row)
        rows.append(row)
    rank = {"HIGH": 0, "MEDIUM": 1, "LOW": 2}
    rows.sort(key=lambda r: (rank[r["risk"]], -r["classes"]["UNRESOLVED"], -r["classes"]["HOST"], -r["functions"],
                             r["library"] or ""))
    return rows


# ---------------------------------------------------------------- markdown

def _fn(imp: dict) -> str:
    return f"`{imp['name']}`" if imp["name"] else f"`{imp['nid']}` (no name)"


def _shad(row: dict) -> str:
    s = row["shadps4"]
    reg = s["impl"] + s["partial"] + s["stub"] + s["unknown"]
    if not reg:
        return "-"
    parts = [f"{s[k]} {k}" for k in ("impl", "partial", "stub", "unknown") if s[k]]
    return f"{reg}/{row['functions']} ({', '.join(parts)})"


def _shad_cell(imp: dict) -> str:
    if not imp["shadps4"]:
        return "-"
    text = imp["shadps4"]
    if imp["name"] is None and imp["shadps4_function"]:
        text += f" as `{imp['shadps4_function']}`"
    return text + ("" if imp["shadps4_lib_match"] else " (other library)")


def render_markdown(rep: dict, title: str | None = None) -> str:
    s = rep["summary"]
    inp = rep["inputs"]
    distinct = list(_distinct(rep["imports"]).values())
    sites: dict[tuple, set] = defaultdict(set)
    for i in rep["imports"]:
        sites[(i["library"], i["nid"])].add(i["file"])

    out = [f"# Import coverage: {title or Path(rep['app0']).name}", "",
           "Generated by `ghostldr/tools/import_report.py`. Classes follow resolve.c's order: core HLE, "
           "extension HLE, the title's own modules, host system modules (loaded by name, bound with dlsym "
           "by function name), else a logging stub that returns 0 (UNRESOLVED). HOST means the module file "
           "exists on the console; whether it exports each name, and works inside SWRR, isn't checked.", ""]
    out.append(f"- Name database: {inp['names']} names. Core HLE entries: {inp['core_hle_entries']}. "
               f"Extension HLE entries: {inp['ext_hle_entries']}.")
    out.append(f"- System modules a PS4 app can load: {inp['system_modules_visible']} "
               f"(in {', '.join(APP_LIB_DIRS)}).")
    if rep.get("executable_memory", {}).get("verdict"):
        out.append(f"- Executable memory: {exec_fit.summary(rep['executable_memory'])}.")
    if inp["shadps4"]:
        sh = inp["shadps4"]
        out.append(f"- shadPS4: {sh['registrations']} registered functions"
                   + (f" (commit {sh['commit']})" if sh["commit"] else "") + ".")
    out.append("")

    out += ["## Summary", "", "| Class | Functions | Import sites |", "|---|--:|--:|"]
    for c in CLASSES:
        out.append(f"| {c} | {s['functions_by_class'][c]} | {s['sites_by_class'][c]} |")
    out.append(f"| **total** | **{s['functions']}** | **{s['import_sites']}** |")
    out += ["", f"A function is a distinct (library, NID) pair; one imported by several files counts once, with "
            f"its most severe class. {s['unnamed']} NIDs have no name in the database.", ""]

    out += ["## Modules", "", "| File | Module | Imports | Exports | Export libraries | Loaded by boot.c |",
            "|---|---|--:|--:|---|---|"]
    for m in rep["modules"]:
        if m["file"] == "eboot.bin":
            loaded = "main"
        else:
            loaded = {True: "yes", False: "**no**", None: "?"}[m["loaded_by_boot"]]
        out.append(f"| {m['file']} | {m['module'] or '?'} | {m['imports']} | {m['exports']} | "
                   f"{', '.join(m['export_libs']) or '-'} | {loaded} |")
    out.append("")

    fit = rep.get("executable_memory")
    if fit:
        out += ["## Executable memory", "",
                "Where the title's code would run inside SWRR on a PS5 (`tools/exec_fit.py`, the loader's own "
                "rules: arena.c, bigexec.c and bxplan.c).", ""]
        out += [f"**{exec_fit.summary(fit)}**", ""] if "verdict" in fit else [f"**not checked**: {fit['error']}", ""]
        out += ["```", *exec_fit.lines(fit)[1:], "```", ""] if "verdict" in fit else []

    out += ["## Libraries by risk", "",
            "HIGH: some imports resolve to nothing. MEDIUM: some imports bind to the console's own module. "
            "LOW: all HLE or the title's own code. Host file: what resolve.c would load for the library's "
            "module (*unused* when nothing binds to it). shadPS4: registered/imported functions, and how many "
            "of those it implements, partially implements or stubs.", "",
            "| Risk | Library | Module | Fns | CORE | EXT | TITLE | HOST | UNRES | Host file | shadPS4 |",
            "|---|---|---|--:|--:|--:|--:|--:|--:|---|---|"]
    for r in rep["libraries"]:
        c = r["classes"]
        host = ", ".join(r["host_files"])
        if host and not c["HOST"]:
            host += " *unused*"
        if r["host_missing"]:
            host = (host + ", " if host else "") + "**missing**"
        out.append(f"| {r['risk']} | {r['library'] or '?'} | {', '.join(r['modules'])} | {r['functions']} | "
                   f"{c['CORE-HLE'] or ''} | {c['EXT-HLE'] or ''} | {c['TITLE'] or ''} | {c['HOST'] or ''} | "
                   f"{c['UNRESOLVED'] or ''} | {host or '-'} | {_shad(r)} |")
    out.append("")

    unres = sorted((i for i in distinct if i["cls"] == "UNRESOLVED"),
                   key=lambda i: (i["library"] or "", i["name"] or i["nid"]))
    out += [f"## UNRESOLVED ({len(unres)})", ""]
    if unres:
        out += ["| Library | Module | Function | Imported by | shadPS4 | Why |", "|---|---|---|---|---|---|"]
        for i in unres:
            out.append(f"| {i['library'] or '?'} | {i['module'] or '?'} | {_fn(i)} | "
                       f"{', '.join(sorted(sites[(i['library'], i['nid'])]))} | {_shad_cell(i)} | {i['reason']} |")
    else:
        out.append("Nothing.")
    out.append("")

    missing: dict[str, dict] = defaultdict(lambda: {"libs": set(), "unres": 0, "covered": 0})
    for i in distinct:
        if i["host_file"] is None and i["module"] and i["module"] != "libkernel" and i["cls"] != "TITLE":
            m = missing[i["module"]]
            m["libs"].add(i["library"] or "?")
            m["unres" if i["cls"] == "UNRESOLVED" else "covered"] += 1
    out += [f"## Missing host modules ({len(missing)})", ""]
    if missing:
        out += ["Modules the title imports from (outside its own sce_module) with no file a PS4 app can load.", "",
                "| Module | Libraries | Unresolved | Covered by HLE |", "|---|---|--:|--:|"]
        for mod, v in sorted(missing.items(), key=lambda kv: (-kv[1]["unres"], kv[0])):
            out.append(f"| {mod} | {', '.join(sorted(v['libs']))} | {v['unres']} | {v['covered']} |")
    else:
        out.append("None: every system module the title imports from has a file a PS4 app can load.")
    out.append("")

    linked = rep["linked_files"]
    absent = [r for r in linked if not r["present"]]
    out += [f"## Linked system modules ({len(linked) - len(absent)} of {len(linked)} on the console)", "",
            "DT_NEEDED files outside the title's sce_module, checked against the system modules a PS4 app "
            "can load.", ""]
    if absent:
        out += ["Not on the console:", ""]
        for r in absent:
            where = f" (only in {', '.join(r['elsewhere'])})" if r["elsewhere"] else ""
            out.append(f"- {r['linked']}: no {r['file']}{where}; linked by {', '.join(r['by'])}")
        out.append("")
    present = [r["file"] for r in linked if r["present"]]
    if present:
        out += ["On the console: " + ", ".join(present) + ".", ""]

    if rep["module_files"]:
        out += ["## Module names that aren't file names", "",
                "System modules whose import name differs from the file the title links (DT_NEEDED) or the "
                "file resolve.c loads.", "", "| Module | Title links | resolve.c loads | Import sites | Notes |",
                "|---|---|---|---|---|"]
        for r in rep["module_files"]:
            classes = ", ".join(f"{k} {v}" for k, v in sorted(r["sites_by_class"].items()))
            out.append(f"| {r['module']} | {', '.join(r['linked']) or '-'} | {r['loads'] or '**none**'} | "
                       f"{classes} | {'; '.join(r['notes']) or '-'} |")
        out.append("")

    title_warn = [i for i in distinct if i["cls"] == "TITLE" and i["reason"]]
    if title_warn:
        out += ["## Title exports from modules boot.c doesn't load", ""]
        for prov in sorted({i["provider"] for i in title_warn}):
            out.append(f"- {prov}: {sum(1 for i in title_warn if i['provider'] == prov)} functions")
        out.append("")

    if inp["shadps4"]:
        out += ["## shadPS4 status of HOST and UNRESOLVED imports", "",
                "What shadPS4 does with the functions that bind to the console's module or to nothing. A shadPS4 "
                "stub suggests titles get by with a trivial return; *not registered* means shadPS4 has no HLE "
                "for it (it may load the real module instead). Implemented functions are only counted.", ""]
        groups: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))
        for i in distinct:
            if i["cls"] in ("HOST", "UNRESOLVED"):
                groups[i["library"] or "?"][i["shadps4"] or "not registered"].append(i)
        order = ("stub", "partial", "unknown", "not registered", "impl")

        def weight(lib: str) -> tuple:
            g = groups[lib]
            return (-len(g["stub"]) - len(g["partial"]), -len(g["not registered"]), lib)

        for lib in sorted(groups, key=weight):
            g = groups[lib]
            counts = ", ".join(f"{len(g[k])} {k}" for k in order if g.get(k))
            out.append(f"- **{lib}** ({counts})")
            for k in order[:-1]:
                if g.get(k):
                    names = ", ".join(_fn(i) for i in sorted(g[k], key=lambda i: i["name"] or i["nid"]))
                    out.append(f"  - {k}: {names}")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ---------------------------------------------------------------- main

def executable_memory(app0: Path) -> dict:
    """exec_fit's verdict (with the title profile's code ranges when they
    apply), or {"error": why}."""
    try:
        return exec_fit.fit(app0, exec_fit.profile_exec(app0))
    except Exception as e:  # a module the core could not parse either
        return {"error": f"{type(e).__name__}: {e}"}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app0", type=Path, help="the title's app0 directory (eboot.bin, sce_module/)")
    ap.add_argument("--names", type=Path, action="append",
                    help="name list(s), one symbol per line (default: build/nid/known_names.txt "
                         "and ghostldr/data/extra_names.txt)")
    ap.add_argument("--native", type=Path, default=ROOT / "ghostldr" / "native", help="ghostldr/native")
    ap.add_argument("--ext", type=Path, action="append",
                    help="extension HLE sources or lists (default: <native>/ext if it exists)")
    ap.add_argument("--sysmodules", type=Path, default=ROOT / "ghostldr" / "data" / "ps5_ps4_sysmodules.txt")
    ap.add_argument("--shadps4", type=Path, default=ROOT / "build" / "ref" / "shadPS4",
                    help="a shadPS4 source tree (skipped if missing)")
    ap.add_argument("--title", help="report heading (default: the app0 directory's name)")
    ap.add_argument("--md", type=Path, help="write the markdown report here (default: stdout)")
    ap.add_argument("--json", type=Path, help="write the JSON report here")
    args = ap.parse_args(argv)

    name_files = args.names or [ROOT / "build" / "nid" / "known_names.txt",
                                ROOT / "ghostldr" / "data" / "extra_names.txt"]
    names = oe.load_name_db(*name_files)
    if not names:
        print(f"warning: no names loaded from {', '.join(map(str, name_files))}", file=sys.stderr)
    core = load_core_hle(args.native)
    ext = load_ext_hle(args.ext if args.ext else [p for p in [args.native / "ext"] if p.exists()])
    sysmods = load_sysmodules(args.sysmodules)
    shad = ShadPS4(args.shadps4) if (args.shadps4 / "src" / "core" / "libraries").is_dir() else None
    if shad is None:
        print(f"note: no shadPS4 tree at {args.shadps4}; skipping its column", file=sys.stderr)
    bundled = load_bundled(args.native)
    if bundled is None and (args.app0 / "eboot.bin").is_file():
        import gen_nids  # the boot's own choice (modules.c): Fios2, libc and what the imports need
        bundled = [f.name for f in gen_nids.boot_modules(args.app0)]
    rep = build_report(args.app0, names, core, ext, sysmods, shad, bundled)
    if (args.app0 / "eboot.bin").is_file():
        rep["executable_memory"] = executable_memory(args.app0)
    md = render_markdown(rep, args.title)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(rep, indent=1) + "\n")
    if args.md:
        args.md.parent.mkdir(parents=True, exist_ok=True)
        args.md.write_text(md)
    else:
        sys.stdout.write(md)
    s = rep["summary"]
    print("functions: " + ", ".join(f"{c} {s['functions_by_class'][c]}" for c in CLASSES)
          + f" (total {s['functions']}, {s['import_sites']} sites)", file=sys.stderr)
    fit = rep.get("executable_memory")
    if fit:
        print("executable memory: " + (exec_fit.summary(fit) if "verdict" in fit else f"not checked ({fit['error']})"),
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
