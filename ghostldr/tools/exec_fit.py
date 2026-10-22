#!/usr/bin/env python3
"""Does a title's code fit the executable memory ghostldr has inside SWRR?

ghostldr runs a title's code in the PS2 emulator's JIT buffers (a stock PS5,
no kernel exploit: no other executable memory).  On 13.60 they are one
32 MiB buffer (EE), one of 16 MiB and two of 8 MiB, plus the small one that
holds the core.  This answers, before anything is sent to the console, with
the rules the core uses:

  fits the EE buffer          arena.c: the boot prx (libSceFios2, libc, then
                              those whose libraries are imported, by name)
                              each at the next 64 KiB boundary after the
                              previous module's whole image, then eboot.bin;
                              every module's executable segment and RELRO
                              inside the EE buffer, from 16 KiB in.  Data
                              segments go outside (system flexible memory).
  needs multi-buffer placement  bigexec.c: the module with the most text gets
                              whole buffers of its own, mapped back to back
                              over its code (bxplan.c's code map: .eh_frame,
                              prologues, PLT, entry points, margins); the rest
                              of its segments are direct memory, and the
                              other modules use the largest buffer left.
  won't fit                   neither.  When fresh JIT objects sized to the
                              code would fit once the emulator's buffers are
                              reclaimed (not done yet), it
                              says "would fit after reclaim".

The plan itself (code_runs, cover) is a line-by-line twin of
native/src/bxplan.c, so the verdict and the loader agree: change both
together (tests/test_bigexec.py runs both on the same inputs).

    python3 ghostldr/tools/exec_fit.py APP0 [--json]
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orbis_elf as oe  # noqa: E402
from gen_nids import boot_modules, module_candidates  # noqa: E402

MiB = 1 << 20
CHUNK, JOIN, PAD = 0x10000, 0x100000, 0x40000      # bxplan.h BX_*
MAX_RUNS, MAX_OBJS, MAX_FORBID = 6, 5, 8
TEXT_MAX = 512 * MiB
TMP_RUNS = 32
JIT_BUF_HEADER = 0x4000                             # ghost.h: never written in a buffer's first 16 KiB
# the emulator's JIT buffers on a PS5 (13.60, measured): home address, size;
# the IPU buffer, which holds the core, is not one of them
BUFFERS = [(0x900a1c000, 32 * MiB), (0x902a20000, 8 * MiB), (0x903224000, 8 * MiB), (0x903a28000, 16 * MiB)]
NAMES = {0x900a1c000: "EE", 0x902a20000: "IOP", 0x903224000: "VU0", 0x903a28000: "VU1"}

FITS, MULTI, NO_FIT = "fits the EE buffer", "needs multi-buffer placement", "won't fit"
# Not implemented, only reported: with the emulator's four buffers reclaimed
# (every descriptor and view of them dropped), fresh JIT
# objects of any 16 KiB multiple fit in the 65 MiB cap less the core's
# 256 KiB object.  The extension's code takes a block of its own (320 KiB
# today).
RECLAIM_BUDGET = 65 * MiB - 256 * 1024
EXT_JIT = 320 * 1024


def align_up(x: int, a: int) -> int:
    return (x + a - 1) & ~(a - 1)


def align_down(x: int, a: int) -> int:
    return x & ~(a - 1)


def s32(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


# ---- bxplan.c, line by line ----

def _ld32(b: bytes, i: int) -> int:
    return struct.unpack_from("<I", b, i)[0]


class CodeIn:
    """struct bx_code_in"""

    def __init__(self, seg: bytes, seg_vaddr: int, text_size: int, eh_hdr: int = 0, points: tuple = (),
                 got: tuple[int, int] = (0, 0), exec_ranges: list | None = None) -> None:
        self.seg, self.seg_vaddr, self.seg_filesz = seg, seg_vaddr, len(seg)
        self.text_size, self.eh_hdr, self.points = text_size, eh_hdr, list(points)
        self.got_lo, self.got_hi = got
        self.exec = exec_ranges or []


def _seg_at(i: CodeIn, v: int, n: int) -> int | None:
    """offset into seg of [v, v + n), or None"""
    if v < i.seg_vaddr or n > i.seg_filesz or v - i.seg_vaddr > i.seg_filesz - n:
        return None
    return v - i.seg_vaddr


def _enc_size(enc: int) -> int:
    return {0x0: 8, 0x4: 8, 0xC: 8, 0x3: 4, 0xB: 4, 0x2: 2, 0xA: 2}.get(enc & 0x0F, 0)


def _ld_n(b: bytes, p: int, n: int) -> int:
    return struct.unpack_from({8: "<Q", 4: "<I", 2: "<H"}[n], b, p)[0]


def _skip_leb(b: bytes, p: int, end: int) -> int:
    while p < end:
        c = b[p]
        p += 1
        if not c & 0x80:
            break
    return p


def _cie_enc(i: CodeIn, cie: int) -> int:
    h = _seg_at(i, cie, 9)
    seg = i.seg
    ln = _ld32(seg, h) if h is not None else 0
    if h is None or ln < 5 or ln == 0xFFFFFFFF or _seg_at(i, cie, ln + 4) is None or _ld32(seg, h + 4) != 0:
        return -1
    end, aug = h + 4 + ln, h + 9
    p = aug
    while p < end and seg[p]:
        p += 1
    if p >= end:
        return -1
    p += 1
    if seg[aug] != ord("z"):
        return 0
    p = _skip_leb(seg, p, end)
    p = _skip_leb(seg, p, end)
    p = p + 1 if seg[h + 8] == 1 else _skip_leb(seg, p, end)
    p = _skip_leb(seg, p, end)
    a = aug + 1
    while seg[a]:
        if p >= end:
            return -1
        c = chr(seg[a])
        if c == "R":
            return seg[p]
        if c == "L":
            p += 1
        elif c == "P":
            n = _enc_size(seg[p])
            if not n or (seg[p] & 0x70) == 0x50:
                return -1
            p += 1 + n
        elif c not in "SB":
            return -1
        a += 1
    return 0


class Stats:
    def __init__(self) -> None:
        self.fdes = self.prologues = self.plt = self.points = self.fde_bytes = 0
        self.eh = False


def _mark(chunks: set, text: int, lo: int, hi: int) -> None:
    if lo >= text or hi <= lo:
        return
    hi = min(hi, text)
    chunks.update(range(lo // CHUNK, (hi - 1) // CHUNK + 1))


def _mark_fdes(i: CodeIn, chunks: set, st: Stats) -> None:
    seg = i.seg
    h = _seg_at(i, i.eh_hdr, 4) if i.eh_hdr else None
    en = _enc_size(seg[h + 1]) if h is not None else 0
    if (h is None or seg[h] != 1 or seg[h + 3] != 0x3B or not en or _enc_size(seg[h + 2]) != 4
            or _seg_at(i, i.eh_hdr + 4 + en, 4) is None):
        return
    count, tab = _ld32(seg, h + 4 + en), i.eh_hdr + 8 + en
    t = _seg_at(i, tab, count * 8)
    if t is None:
        return
    st.eh = True
    last, enc = None, -1
    for k in range(count):
        loc_rel, fde_rel = struct.unpack_from("<ii", seg, t + 8 * k)
        loc, fde = (i.eh_hdr + loc_rel) % 2**64, (i.eh_hdr + fde_rel) % 2**64
        f = _seg_at(i, fde, 8)
        if f is None:
            continue
        cie = (fde + 4 - _ld32(seg, f + 4)) % 2**64
        if cie != last:
            last, enc = cie, _cie_enc(i, cie)
        n = _enc_size(enc) if enc >= 0 else 0
        r = _seg_at(i, fde + 8 + n, n) if n else None
        rng = _ld_n(seg, r, n) if r is not None else 0
        if not rng:
            continue
        st.fdes += 1
        st.fde_bytes += rng
        _mark(chunks, i.text_size, loc, loc + rng)


def _join_closest(r: list) -> None:
    best = 1
    for k in range(2, len(r)):
        if r[k][0] - r[k - 1][1] < r[best][0] - r[best - 1][1]:
            best = k
    r[best - 1][1] = r[best][1]
    del r[best]


def code_runs(i: CodeIn, st: Stats | None = None) -> list[tuple[int, int]]:
    """bx_code_runs: the code runs [lo, hi), sorted, at most MAX_RUNS."""
    st = st if st is not None else Stats()
    text = i.text_size
    tmp: list[list[int]] = []
    if not text or text > TEXT_MAX:
        return []
    if i.exec:
        for a, b in i.exec:
            lo, hi = align_down(a, 0x4000), min(align_up(b, 0x4000), text)
            if lo >= hi:
                continue
            if tmp and lo <= tmp[-1][1]:
                tmp[-1][1] = max(tmp[-1][1], hi)
                continue
            if len(tmp) == TMP_RUNS:
                _join_closest(tmp)
            tmp.append([lo, hi])
        while len(tmp) > MAX_RUNS:
            _join_closest(tmp)
        return [tuple(r) for r in tmp]
    chunks: set[int] = set()
    _mark_fdes(i, chunks, st)
    seg, base = i.seg, i.seg_vaddr
    for m in re.finditer(b"\x55\x48\x89\xe5", seg):
        v = base + m.start()
        if v % 16 == 0:
            st.prologues += 1
            _mark(chunks, text, v, v + 1)
    if i.got_hi > i.got_lo:
        for m in re.finditer(b"\xff\x25", seg):
            k = m.start()
            if k + 6 > len(seg):
                break
            t = (base + k + 6 + s32(_ld32(seg, k + 2))) % 2**64
            if i.got_lo <= t < i.got_hi:
                st.plt += 1
                _mark(chunks, text, base + k, base + k + 6)
    for p in i.points:
        st.points += 1
        _mark(chunks, text, p, p + 1)
    for c in sorted(chunks):
        lo, hi = c * CHUNK, (c + 1) * CHUNK
        if tmp and lo - tmp[-1][1] < JOIN:
            tmp[-1][1] = hi
            continue
        if len(tmp) == TMP_RUNS:
            _join_closest(tmp)
        tmp.append([lo, hi])
    while len(tmp) > MAX_RUNS:
        _join_closest(tmp)
    runs: list[list[int]] = []
    for lo, hi in tmp:
        lo, hi = max(lo - PAD, 0), min(hi + PAD, text)
        if runs and lo <= runs[-1][1]:
            runs[-1][1] = hi
        else:
            runs.append([lo, hi])
    return [tuple(r) for r in runs]


def cover(runs: list, forbid: list, obj: list[int], keep: int) -> dict | None:
    """bx_cover: {"views": [(object index, offset)], "used": bytes}, or None."""
    nruns = len(runs)
    if not nruns or nruns > MAX_RUNS or len(obj) > MAX_OBJS or len(forbid) > MAX_FORBID:
        return None
    best_used, best_n, best_mask, best = None, None, None, None
    nobj = len(obj)
    for mask in range(1 << (nruns - 1)):
        ga, gb = [], []
        for k in range(nruns):
            if not k or (mask >> (k - 1)) & 1:
                ga.append(runs[k][0])
                gb.append(0)
            gb[-1] = runs[k][1]
        groups = len(ga)
        if groups > nobj:
            continue
        for code in range((groups + 1) ** nobj):
            d, t, used, nused, umask, spare, c = [], [0] * groups, 0, 0, 0, 0, code
            for j in range(nobj):
                d.append(c % (groups + 1))
                c //= groups + 1
                if d[j] < groups:
                    t[d[j]] += obj[j]
                    used += obj[j]
                    nused += 1
                    umask |= 1 << j
                elif obj[j] > spare:
                    spare = obj[j]
            if best_used is not None and (used > best_used or (used == best_used and (
                    nused > best_n or (nused == best_n and umask >= best_mask)))):
                continue
            if spare < keep:
                continue
            at, prev, ok = [], -TEXT_MAX, True
            for g in range(groups):
                lo, stop = gb[g] - t[g], 2**63 - 1
                ok = t[g] >= gb[g] - ga[g]
                lo = max(lo, prev)
                for flo, fhi in forbid:
                    if fhi <= ga[g]:
                        lo = max(lo, fhi)
                    elif flo >= gb[g]:
                        stop = min(stop, flo)
                    else:
                        ok = False
                s = max(lo, 0)
                if s > ga[g] or s + t[g] > stop:
                    s = lo
                ok = ok and s <= ga[g] and s + t[g] <= stop
                at.append(s)
                prev = s + t[g]
                if not ok:
                    break
            if not ok:
                continue
            best_used, best_n, best_mask = used, nused, umask
            views = []
            for g in range(groups):
                o = at[g]
                for j in range(nobj):
                    if d[j] == g:
                        views.append((j, o))
                        o += obj[j]
            best = {"views": views, "used": used}
    return best


# ---- a module as the core sees it (elf.c) ----

class Module:
    def __init__(self, path: Path) -> None:
        img = oe.parse(path)
        self.name, self.img = path.name, img
        loads = [p for p in img.phdrs if p.type in (oe.PT_LOAD, oe.PT_SCE_RELRO)]
        lo = min(p.vaddr for p in loads)
        hi = max(p.vaddr + p.memsz for p in loads)
        self.image_size = align_up(hi, 0x4000) - align_down(lo, 0x4000)
        self.text = None
        for p in img.phdrs:
            if p.type == oe.PT_LOAD and p.flags & 1:
                self.text = p
        self.text_size = align_up(self.text.vaddr + self.text.memsz, 0x4000) if self.text else 0
        self.relro_end = max((align_up(p.vaddr + p.memsz, 0x4000) for p in img.phdrs if p.type == oe.PT_SCE_RELRO),
                             default=0)
        self.data = [(align_down(p.vaddr, 0x4000), align_up(p.vaddr + p.memsz, 0x4000)) for p in img.phdrs
                     if ((p.type == oe.PT_LOAD and not p.flags & 1) or p.type == oe.PT_SCE_RELRO) and p.memsz]
        self.fingerprint = img.fingerprint.hex()

    def forbid(self) -> list[tuple[int, int]]:
        """bigexec.c data_ranges: sorted, merged"""
        out: list[list[int]] = []
        for lo, hi in sorted(self.data[:MAX_FORBID], key=lambda r: r[0]):
            if out and lo <= out[-1][1]:
                out[-1][1] = max(out[-1][1], hi)
            else:
                out.append([lo, hi])
        return [tuple(r) for r in out]

    def data_bytes(self) -> int:
        return sum(hi - lo for lo, hi in self.forbid())

    def code_in(self, exec_ranges: list | None = None) -> CodeIn:
        img, t = self.img, self.text
        seg = img.elf[t.offset:t.offset + t.filesz]
        eh = img.phdr(oe.PT_GNU_EH_FRAME)
        points = [img.entry]
        if img.dyn_value(oe.DT_INIT) is not None:
            points.append(img.dyn_value(oe.DT_INIT))
        if img.dyn_value(oe.DT_FINI):
            points.append(img.dyn_value(oe.DT_FINI))
        offs = [r.offset for r in img.jmprels]
        got = (min(offs), max(offs) + 8) if offs else (0, 0)
        return CodeIn(seg, t.vaddr, self.text_size, eh.vaddr if eh else 0, points, got, exec_ranges)


# ---- the fit check ----

def _fits_at(m: Module, at: int, hi: int) -> bool:
    return at + m.text_size <= hi and at + m.relro_end <= hi


def arena_place(mods: list[Module], region: tuple[int, int]) -> tuple[bool, int, list]:
    """boot.c's arena placement (the executable last): (all fit, the next free
    base for a run-time load, [(module, base)])"""
    lo, hi = region[0] + JIT_BUF_HEADER, region[0] + region[1]
    at, placed, ok = align_up(lo, 0x10000), [], True
    for m in [m for m in mods if m.name != "eboot.bin"] + [m for m in mods if m.name == "eboot.bin"]:
        ok = ok and _fits_at(m, at, hi)
        placed.append((m.name, at))
        at = align_up(at + m.image_size, 0x10000)
    return ok, at, placed


def fit(app0: Path, exec_ranges: dict[str, list] | None = None) -> dict:
    """The verdict for an extracted app0 (eboot.bin, sce_module/); exec_ranges:
    a profile's code ranges per module file (on its own build only)."""
    app0 = Path(app0)
    boot = boot_modules(app0)
    mods = [Module(f) for f in boot] + [Module(app0 / "eboot.bin")]
    later = []
    for f in module_candidates(app0):
        if f not in boot:
            try:
                later.append(Module(f))
            except Exception:
                pass
    ee = max(BUFFERS, key=lambda b: b[1])
    ok, nxt, placed = arena_place(mods, ee)
    out = {
        "modules": [{"file": m.name, "text": m.text_size, "image": m.image_size, "data": m.data_bytes()} for m in mods],
        "on_demand": [{"file": m.name, "image": m.image_size} for m in later],
        "on_demand_bytes": sum(align_up(m.image_size, 0x10000) for m in later),
    }
    if ok:
        out.update(verdict=FITS, arena=NAMES[ee[0]], placed=[{"file": n, "base": b} for n, b in placed],
                   headroom=max(0, ee[0] + ee[1] - nxt),
                   system_flex=sum(m.data_bytes() for m in mods))
        return out
    big = None                                               # bigexec.c: the most text; ties: the executable
    for m in mods:
        if big is None or m.text_size > big.text_size or (m.text_size == big.text_size and m.name == "eboot.bin"):
            big = m
    rest = [m for m in mods if m is not big]
    keep = 0x10000 + sum(align_up(m.image_size, 0x10000) for m in rest)
    bufs = sorted(BUFFERS, key=lambda b: -b[1])               # stable: by address within a size
    st = Stats()
    ranges = (exec_ranges or {}).get(big.name)
    runs = code_runs(big.code_in(ranges), st)
    lay = cover(runs, big.forbid(), [b[1] for b in bufs], keep) if runs else None
    out.update(big=big.name, runs=runs, keep=keep, code_from="profile" if ranges else "evidence",
               evidence={"fdes": st.fdes, "fde_bytes": st.fde_bytes, "prologues": st.prologues, "plt": st.plt,
                         "eh_frame_hdr": st.eh})
    # what fresh objects sized to the code would need, after a reclaim
    out["reclaim_need"] = sum(align_up(b - a, 0x4000) for a, b in runs) + keep + EXT_JIT if runs else None
    if not lay:
        out.update(verdict=NO_FIT)
        return out
    used = {j for j, _ in lay["views"]}
    left = [b for j, b in enumerate(bufs) if j not in used]
    region = max(sorted(left), key=lambda b: b[1])            # arena.c: the largest, lowest address on ties
    ok, nxt, placed = arena_place(rest, region)
    covered = 0
    for j, off in lay["views"]:
        covered += max(0, min(off + bufs[j][1], big.text_size) - max(off, 0))
    out.update(verdict=MULTI if ok else NO_FIT,
               views=[{"buffer": NAMES[bufs[j][0]], "size": bufs[j][1], "offset": off} for j, off in lay["views"]],
               direct=big.text_size - covered + big.data_bytes(), arena=NAMES[region[0]],
               placed=[{"file": n, "base": b} for n, b in placed], headroom=max(0, region[0] + region[1] - nxt),
               system_flex=sum(m.data_bytes() for m in rest))
    return out


def summary(r: dict) -> str:
    """The one-line verdict."""
    if r["verdict"] == FITS:
        return f"{FITS} (code {sum(m['text'] for m in r['modules']) / MiB:.1f} MiB)"
    if r["verdict"] == MULTI:
        v = " + ".join(f"{x['buffer']} {x['size'] // MiB}" for x in r["views"])
        return f"{MULTI} ({r['big']}: {v} MiB buffers over its code, {r['direct'] / MiB:.1f} MiB in direct memory)"
    code = sum(b - a for a, b in r.get("runs", []))
    need = r.get("reclaim_need")
    if need and need <= RECLAIM_BUDGET:
        return (f"{NO_FIT} with buffer views; would fit after reclaim ({r['big']}: {code / MiB:.1f} MiB of code, "
                f"{need / MiB:.1f} of {RECLAIM_BUDGET / MiB:.2f} MiB of JIT memory with the other modules)")
    return (f"{NO_FIT} ({r.get('big', 'eboot.bin')}: {code / MiB:.1f} MiB of code; even with the emulator's buffers "
            f"reclaimed there are {RECLAIM_BUDGET / MiB:.2f} MiB of JIT memory)")


def lines(r: dict) -> list[str]:
    """The report section, as text lines."""
    out = [f"verdict: {summary(r)}"]
    for m in r["modules"]:
        out.append(f"  {m['file']}: text {m['text'] / MiB:.2f} MiB, image {m['image'] / MiB:.2f} MiB, "
                   f"data {m['data'] / MiB:.2f} MiB")
    if "runs" in r:
        ev = r["evidence"]
        how = ("the profile's ranges" if r["code_from"] == "profile" else
               f"{ev['fdes']} FDEs, {ev['prologues']} prologues, {ev['plt']} PLT entries")
        out.append(f"  code of {r['big']} ({how}): " +
                   ", ".join(f"+{a:x}-+{b:x}" for a, b in r["runs"]))
    for v in r.get("views", []):
        out.append(f"  {v['buffer']} ({v['size'] // MiB} MiB) at +{v['offset']:x}-+{v['offset'] + v['size']:x}")
    if "direct" in r:
        out.append(f"  direct memory for the rest of {r['big']} (read-only data and data): {r['direct'] / MiB:.1f} MiB,"
                   " out of the title's budget")
    if "arena" in r:
        who = "the other modules" if "views" in r else "the modules"
        out.append(f"  {who} in the {r['arena']} buffer; their data segments from system flexible memory: "
                   f"{r['system_flex'] / MiB:.1f} MiB")
        need = r["on_demand_bytes"]
        out.append(f"  room left there for prx loaded at run time: {r['headroom'] / MiB:.1f} MiB"
                   + (f" (sce_module files loaded on demand: {need / MiB:.1f} MiB"
                      + (", they fit)" if need <= r["headroom"] else ", not all of them fit)") if need else ""))
    return out


def profile_exec(app0: Path, title: str | None = None) -> dict[str, list]:
    """The title profile's "exec" ranges when they apply to this build."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
    try:
        import profiles
        title = title or profiles.detect_title(app0)
        prof = profiles.load(title) if title else {}
    except Exception:
        return {}
    fp = prof.get("eboot", {}).get("fingerprint", "")
    if not prof.get("exec") or not fp or Module(Path(app0) / "eboot.bin").fingerprint != fp.lower():
        return {}
    return profiles.exec_ranges(prof)


def check(app0: Path, title: str | None = None) -> tuple[dict | None, str]:
    """For gfs_pack.py and gfs_install.py: (the verdict or None, a line to print)."""
    try:
        r = fit(Path(app0), profile_exec(Path(app0), title))
    except Exception as e:  # unreadable modules: the core could not load them either
        return None, f"executable memory: not checked ({type(e).__name__}: {e})"
    return r, f"executable memory: {summary(r)}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app0", type=Path)
    ap.add_argument("--title", help="its title ID (default: from the dump), for the profile's code ranges")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    r = fit(args.app0, profile_exec(args.app0, args.title))
    print(json.dumps(r, indent=1) if args.json else "\n".join(lines(r)))
    return 0 if r["verdict"] != NO_FIT else 1


if __name__ == "__main__":
    raise SystemExit(main())
