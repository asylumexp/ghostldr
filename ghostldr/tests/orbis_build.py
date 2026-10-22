"""Tiny loadable Orbis modules for the offline loader tests (ext_host kernel
mode): a plain ELF the core's module_parse accepts, with an executable
PT_LOAD at 0, a data PT_LOAD after it, PT_DYNAMIC and PT_SCE_DYNLIBDATA
(string, symbol and relocation tables, module and library ids), and
optionally PT_TLS, PT_GNU_EH_FRAME and PT_SCE_PROCPARAM.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import orbis_elf as oe  # noqa: E402

PAGE = 0x4000
R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE = 8
R_X86_64_DTPMOD64 = 16
R_X86_64_DTPOFF64 = 17
R_X86_64_TPOFF64 = 18
R_X86_64_DTPOFF32 = 21
R_X86_64_TPOFF32 = 23
STT_TLS = 6


def ret_imm(value: int) -> bytes:
    """mov eax, imm32; ret"""
    return b"\xb8" + struct.pack("<I", value & 0xFFFFFFFF) + b"\xc3"


@dataclass
class Module:
    name: str                                   # SCE module name (DT_SCE_MODULE_INFO)
    e_type: int = oe.ET_SCE_DYNAMIC
    text: bytes = b""                           # at vaddr 0
    data: bytes = b""                           # at the first page after the text
    data_size: int = PAGE
    entry: int = 0
    init: int | None = None                     # DT_INIT (text offset)
    fini: int | None = None
    export_libs: dict[int, str] = field(default_factory=dict)
    import_libs: dict[int, str] = field(default_factory=dict)
    needed_mods: dict[int, str] = field(default_factory=dict)
    exports: list[tuple[str, int, int, int]] = field(default_factory=list)   # name, lib id, value, STT
    imports: list[tuple[str, int, int, int]] = field(default_factory=list)   # name ("=NID": raw), lib, mod, STT
    relas: list[tuple[int, int, int, int]] = field(default_factory=list)     # offset, type, symbol index, addend
    jmprels: list[tuple[int, int]] = field(default_factory=list)             # offset, symbol index
    tls: tuple[int, int, int, int] | None = None                             # vaddr, filesz, memsz, align
    eh_frame_hdr: int | None = None             # text offset of an .eh_frame_hdr (12 bytes, built here)
    eh_frame: int | None = None                 # text offset of its .eh_frame
    eh_keep: bool = False                       # the text already holds both (with FDEs): leave them
    proc_param: int | None = None               # data offset of a SceProcParam
    fingerprint: bytes = bytes(range(1, 21))

    @property
    def text_size(self) -> int:
        return (len(self.text) + PAGE - 1) // PAGE * PAGE or PAGE

    def import_index(self, i: int) -> int:
        """symbol index of import i"""
        return 1 + i

    def export_index(self, i: int) -> int:
        return 1 + len(self.imports) + i

    def build(self) -> bytes:
        strtab = bytearray(b"\0")

        def s(text: str) -> int:
            strtab.extend(text.encode() + b"\0")
            return len(strtab) - len(text) - 1

        enc = oe.ID_ALPHABET.__getitem__

        def nid(name: str) -> str:
            return name[1:] if name.startswith("=") else oe.compute_nid(name)   # "=NID": a raw NID

        text = bytearray(self.text.ljust(self.text_size, b"\xcc"))
        if self.eh_frame_hdr is not None and not self.eh_keep:
            hdr, frame = self.eh_frame_hdr, self.eh_frame or 0
            text[hdr:hdr + 12] = bytes([1, 0x1B, 0x03, 0x3B]) + struct.pack("<iI", frame - (hdr + 4), 0)
            text[frame:frame + 4] = bytes(4)
        syms = [bytes(24)]
        for name, lib, mod, typ in self.imports:
            syms.append(struct.pack("<IBBHQQ", s(f"{nid(name)}#{enc(lib)}#{enc(mod)}"), (oe.STB_GLOBAL << 4) | typ,
                                    0, 0, 0, 0))
        for name, lib, value, typ in self.exports:
            syms.append(struct.pack("<IBBHQQ", s(f"{nid(name)}#{enc(lib)}#A"), (oe.STB_GLOBAL << 4) | typ, 0, 1,
                                    value, 8))
        dyn = [(oe.DT_SCE_MODULE_INFO, (1 << 40) | (1 << 32) | s(self.name))]
        dyn += [(oe.DT_SCE_NEEDED_MODULE, (i << 48) | (1 << 40) | (1 << 32) | s(n)) for i, n in self.needed_mods.items()]
        dyn += [(oe.DT_SCE_IMPORT_LIB, (i << 48) | (1 << 32) | s(n)) for i, n in self.import_libs.items()]
        dyn += [(oe.DT_SCE_EXPORT_LIB, (i << 48) | (1 << 32) | s(n)) for i, n in self.export_libs.items()]
        if self.init is not None:
            dyn.append((oe.DT_INIT, self.init))
        if self.fini is not None:
            dyn.append((oe.DT_FINI, self.fini))
        symtab = b"".join(syms)
        rela = b"".join(struct.pack("<QQq", off, (sym << 32) | typ, add) for off, typ, sym, add in self.relas)
        jmprel = b"".join(struct.pack("<QQq", off, (sym << 32) | R_X86_64_JUMP_SLOT, 0) for off, sym in self.jmprels)
        dynlib = bytearray(strtab)
        sym_off = len(dynlib)
        dynlib += symtab
        rela_off = len(dynlib)
        dynlib += rela
        jmp_off = len(dynlib)
        dynlib += jmprel
        fp_off = len(dynlib)
        dynlib += self.fingerprint
        dyn += [(oe.DT_SCE_STRTAB, 0), (oe.DT_SCE_STRSZ, len(strtab)), (oe.DT_SCE_SYMTAB, sym_off),
                (oe.DT_SCE_SYMTABSZ, len(symtab)), (oe.DT_SCE_RELA, rela_off), (oe.DT_SCE_RELASZ, len(rela)),
                (oe.DT_SCE_JMPREL, jmp_off), (oe.DT_SCE_PLTRELSZ, len(jmprel)),
                (oe.DT_SCE_FINGERPRINT, fp_off), (oe.DT_NULL, 0)]
        dyn_blob = b"".join(struct.pack("<QQ", t, v) for t, v in dyn)

        data = bytes(self.data).ljust(self.data_size, b"\0")
        data_vaddr = self.text_size
        phdrs = []
        text_off = PAGE
        data_off = text_off + len(text)
        tail = data_off + len(data)
        phdrs.append((oe.PT_LOAD, 5, text_off, 0, len(text), len(text), PAGE))
        phdrs.append((oe.PT_LOAD, 6, data_off, data_vaddr, len(data), len(data), PAGE))
        phdrs.append((oe.PT_DYNAMIC, 6, tail, 0, len(dyn_blob), len(dyn_blob), 8))
        phdrs.append((oe.PT_SCE_DYNLIBDATA, 4, tail + len(dyn_blob), 0, len(dynlib), len(dynlib), 16))
        if self.tls:
            va, filesz, memsz, align = self.tls
            phdrs.append((oe.PT_TLS, 4, data_off + (va - data_vaddr), va, filesz, memsz, align))
        if self.eh_frame_hdr is not None:
            phdrs.append((oe.PT_GNU_EH_FRAME, 4, text_off + self.eh_frame_hdr, self.eh_frame_hdr, 12, 12, 4))
        if self.proc_param is not None:
            phdrs.append((oe.PT_SCE_PROCPARAM, 4, data_off + self.proc_param, data_vaddr + self.proc_param, 0x60,
                          0x60, 8))
        ehdr = b"\x7fELF" + bytes([2, 1, 1, 9]) + bytes(8) + struct.pack(
            "<HHIQQQIHHHHHH", self.e_type, 0x3E, 1, self.entry, 0x40, 0, 0, 0x40, 0x38, len(phdrs), 0, 0, 0)
        head = ehdr + b"".join(struct.pack("<IIQQQQQQ", t, f, off, va, va, fs, ms, al)
                               for t, f, off, va, fs, ms, al in phdrs)
        return head.ljust(PAGE, b"\0") + bytes(text) + data + dyn_blob + bytes(dynlib)

    def write(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.build())
        return path


def kernel_title(root: Path) -> Path:
    """The synthetic app0 ext_host's kernel mode loads (tests/ext_host/kernel.c)."""
    func = oe.STT_FUNC
    app0 = root / "app0"
    sm = app0 / "sce_module"
    # mov rax, fs:[0] (relaxed form), add r11, fs:[0], and an unrelated fs:[0x30] load
    fs_forms = (bytes.fromhex("666666644 88b042500000000".replace(" ", "")) + b"\xcc" * 4
                + bytes.fromhex("644c031c2500000000") + b"\xcc" * 7 + bytes.fromhex("64488b3c2530000000"))
    fs_load = bytes.fromhex("64488b042500000000")

    def prx(file: str, module: str, init_code: bytes, export: str, imports: tuple[str, str, str] | None = None,
            **kw) -> Module:
        text = kw.pop("text", init_code.ljust(0x20, b"\xcc") + b"\xc3")
        m = Module(module, text=text, init=kw.pop("init", 0), export_libs={1: module},
                   exports=[(export, 1, kw.pop("export_at", 0x20), func)], **kw)
        if imports:
            lib, mod, name = imports
            m.import_libs, m.needed_mods = {2: lib}, {2: mod}
            m.imports, m.jmprels = [(name, 2, 2, func)], [(m.text_size, 1)]
        m.write(sm / file)
        return m

    prx("libSceFios2.prx", "libSceFios2", ret_imm(0xF1), "fiosFn")
    # libc: dynamic TLS (a tls_index at data+0x10), unwind info, DT_FINI
    prx("libc.prx", "libc", ret_imm(0xC0), "libcFn", ("libSceFoo", "libSceFoo", "fooFn"), fini=0x30,
        eh_frame_hdr=0x100, eh_frame=0x80, tls=(0x4100, 8, 16, 16), data=bytes(0x100) + b"LIBCTLS!",
        relas=[(0x4010, R_X86_64_DTPMOD64, 0, 0), (0x4018, R_X86_64_DTPOFF64, 0, 8)])
    # e_entry would return 0xBAD: the start must go through DT_INIT
    prx("libSceFoo.prx", "libSceFoo", b"", "fooFn", ("libSceZed", "libSceZed", "zedFn"),
        text=ret_imm(0xBAD).ljust(0x10, b"\xcc") + ret_imm(0xF00).ljust(0x10, b"\xcc") + b"\xc3", init=0x10)
    # no TLS: its fs: load is not the loader's business; an unresolvable variable and function
    zed = Module("libSceZed", text=ret_imm(0x2ED).ljust(0x40, b"\xcc") + fs_load, init=0,
                 export_libs={1: "libSceZed"}, exports=[("zedFn", 1, 0x20, func)],
                 import_libs={2: "libSceMissing"}, needed_mods={2: "libSceMissing"},
                 imports=[("missingVariable", 2, 2, oe.STT_OBJECT), ("missingFunction", 2, 2, func)])
    zed.relas = [(0x4010, R_X86_64_GLOB_DAT, 1, 0)]
    zed.jmprels = [(0x4018, 2)]
    zed.write(sm / "libSceZed.prx")
    # initial-exec TLS: a static image, TPOFF64 relocation, fs:[0] patched
    tls = prx("libSceTls.prx", "libSceTls", b"", "tlsFn", text=ret_imm(0x715).ljust(0x40, b"\xcc") + fs_load,
              tls=(0x4100, 4, 0x20, 32), data=bytes(0x100) + b"TLS2", relas=[(0x4020, R_X86_64_TPOFF64, 0, 8)])
    tls.exports.append(("tlsVar", 1, 0x10, STT_TLS))   # a thread_local others import
    tls.write(sm / "libSceTls.prx")
    # imports libSceTls's tlsVar: DTPMOD64/DTPOFF64 name libSceTls's block, TPOFF64 its static offset
    user = Module("libSceTlsUser", text=ret_imm(0x7115E5).ljust(0x20, b"\xcc") + b"\xc3", init=0,
                  export_libs={1: "libSceTlsUser"}, exports=[("tlsUserFn", 1, 0x20, func)],
                  import_libs={2: "libSceTls"}, needed_mods={2: "libSceTls"}, imports=[("tlsVar", 2, 2, STT_TLS)])
    user.relas = [(0x4000, R_X86_64_DTPMOD64, 1, 0), (0x4008, R_X86_64_DTPOFF64, 1, 4),
                  (0x4010, R_X86_64_TPOFF64, 1, 0)]
    user.write(sm / "libSceTlsUser.prx")
    prx("libSceNpToolkit2.prx", "libSceNpToolkit2", ret_imm(0x102), "tk2Fn")
    prx("libSceNpToolkit.prx", "libSceNpToolkit", ret_imm(0xF7), "tkFn")
    # loads at run time: init returns argc, and it imports from the host
    prx("libSceJson.prx", "libSceJson", b"\x89\xf8\xc3", "jsonFn", ("libkernel", "libkernel", "sceKernelUsleep"),
        export_at=0x30)
    prx("libSceLate.prx", "libSceLate", ret_imm(0), "lateFn")
    # initial-exec TLS cannot load after boot
    prx("libSceIe.prx", "libSceIe", ret_imm(0), "ieFn", tls=(0x4100, 0, 8, 8),
        relas=[(0x4020, R_X86_64_TPOFF64, 0, 0)])
    (sm / "notes.txt").write_text("not a module\n")
    main = Module("game", e_type=oe.ET_SCE_DYNEXEC, text=b"\xc3".ljust(0x10, b"\xcc") + fs_forms,
                  import_libs={1: "libkernel", 2: "libSceNpToolkit2", 3: "libc", 4: "libSceTls", 5: "libSceTlsUser"},
                  needed_mods={1: "libkernel", 2: "libSceNpToolkit2", 3: "libc", 4: "libSceTls", 5: "libSceTlsUser"},
                  imports=[("libcFn", 3, 3, func), ("tk2Fn", 2, 2, func), ("sceKernelUsleep", 1, 1, func),
                           ("tlsFn", 4, 4, func), ("tlsUserFn", 5, 5, func)],
                  tls=(0x4200, 8, 0x30, 16), proc_param=0x300,
                  data=(bytes(0x200) + b"MAINTLS!").ljust(0x300, b"\0")
                  + struct.pack("<QIIQ", 0x60, 0x4942524F, 5, 0x08008011).ljust(0x60, b"\0"),
                  relas=[(0x4040, R_X86_64_DTPMOD64, 0, 0), (0x4048, R_X86_64_DTPOFF64, 0, 4)])
    main.jmprels = [(main.text_size + 8 * i, 1 + i) for i in range(5)]
    main.write(app0 / "eboot.bin")
    return app0


def eh_frame(frame_at: int, hdr_at: int, functions: list[tuple[int, int]]) -> tuple[bytes, bytes]:
    """.eh_frame (one CIE "zR" with pcrel sdata4 addresses, an FDE per (start,
    size) function, the terminator) for text offset frame_at, and its
    .eh_frame_hdr (sorted table, datarel sdata4) for hdr_at."""
    cie = struct.pack("<IB", 0, 1) + b"zR\0" + bytes([1, 0x78, 16, 1, 0x1B])
    cie = cie.ljust((len(cie) + 3) & ~3, b"\0")
    out = struct.pack("<I", len(cie)) + cie
    fdes = []
    for start, size in functions:
        at = frame_at + len(out)
        body = struct.pack("<Iii", at + 4 - frame_at, start - (at + 8), size) + b"\0"
        body = body.ljust((len(body) + 3) & ~3, b"\0")
        fdes.append((start, at))
        out += struct.pack("<I", len(body)) + body
    out += b"\0" * 4
    hdr = bytes([1, 0x1B, 0x03, 0x3B]) + struct.pack("<iI", frame_at - (hdr_at + 4), len(fdes))
    for start, at in sorted(fdes):
        hdr += struct.pack("<ii", start - hdr_at, at - hdr_at)
    return out, hdr
