#!/usr/bin/env python3
"""Minimal reader for the plain x86-64 ELF files the native build links
(core.elf, ext.elf): program headers, sections, symbols, RELA entries."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_RELA = 4
R_X86_64_RELATIVE = 8


@dataclass
class Segment:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int


@dataclass
class Section:
    name: str
    type: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    shndx: int
    info: int


class Elf:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise ValueError(f"{path}: not a little-endian ELF64 file")
        self.entry, phoff, shoff = struct.unpack_from("<QQQ", d, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", d, 0x36)
        self.segments = []
        for i in range(phnum):
            t, fl, off, va, _pa, fs, ms, _al = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
            self.segments.append(Segment(t, fl, off, va, fs, ms))
        raw = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
        names = raw[shstrndx][4] if shnum else 0
        self.sections = []
        for name, t, _fl, addr, off, size, link, _info, _al, entsize in raw:
            self.sections.append(Section(self._str(names, name), t, addr, off, size, link, entsize))
        self.symbols: dict[str, Symbol] = {}
        for sec in self.sections:
            if sec.type != SHT_SYMTAB:
                continue
            strtab = self.sections[sec.link].offset
            for i in range(sec.size // 24):
                name, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, sec.offset + i * 24)
                sym = Symbol(self._str(strtab, name), value, size, shndx, info)
                if sym.name:
                    self.symbols.setdefault(sym.name, sym)

    def _str(self, base: int, off: int) -> str:
        end = self.data.index(b"\0", base + off)
        return self.data[base + off:end].decode()

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def relas(self) -> list[tuple[int, int, int, int]]:
        """(offset, type, symbol index, addend) of every SHT_RELA section."""
        out = []
        for sec in self.sections:
            if sec.type == SHT_RELA:
                for i in range(sec.size // 24):
                    off, info, addend = struct.unpack_from("<QQq", self.data, sec.offset + i * 24)
                    out.append((off, info & 0xFFFFFFFF, info >> 32, addend))
        return out

    def image(self) -> tuple[bytearray, int]:
        """Loadable segments laid out from address 0: (bytes up to the last
        file byte, end of memory)."""
        loads = [s for s in self.segments if s.type == PT_LOAD]
        filesz = max(s.vaddr + s.filesz for s in loads)
        memsz = max(s.vaddr + s.memsz for s in loads)
        img = bytearray(filesz)
        for s in loads:
            img[s.vaddr:s.vaddr + s.filesz] = self.data[s.offset:s.offset + s.filesz]
        return img, memsz
