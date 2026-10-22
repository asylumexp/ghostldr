"""import_report.py against a synthetic title: two tiny Orbis ELFs, a fake
ghostldr/native with HLE tables, a sysmodule list and a fake shadPS4 tree.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import import_report as ir  # noqa: E402
import orbis_elf as oe  # noqa: E402

NID = oe.compute_nid


def make_elf(path: Path, e_type: int, module: str, needed: list[str], mods: dict[int, str],
             libs: dict[int, str], export_libs: dict[int, str],
             imports: list[tuple[str, int, int, int]], exports: list[tuple[str, int]]) -> None:
    """An ELF with only what orbis_elf.parse reads: PT_DYNAMIC and
    PT_SCE_DYNLIBDATA (string table, then symbol table)."""

    strtab = bytearray(b"\0")

    def s(text: str) -> int:
        strtab.extend(text.encode() + b"\0")
        return len(strtab) - len(text) - 1

    enc = oe.ID_ALPHABET.__getitem__
    syms = [bytes(24)]
    for nid, lib, mod, typ in imports:
        syms.append(struct.pack("<IBBHQQ", s(f"{nid}#{enc(lib)}#{enc(mod)}"), (oe.STB_GLOBAL << 4) | typ, 0, 0, 0, 0))
    for nid, lib in exports:
        syms.append(struct.pack("<IBBHQQ", s(f"{nid}#{enc(lib)}#A"), (oe.STB_GLOBAL << 4) | oe.STT_FUNC, 0, 1,
                                0x1000, 16))
    dyn = [(oe.DT_NEEDED, s(f)) for f in needed]
    dyn.append((oe.DT_SCE_MODULE_INFO, (1 << 40) | (1 << 32) | s(module)))
    dyn += [(oe.DT_SCE_NEEDED_MODULE, (i << 48) | (1 << 40) | (1 << 32) | s(n)) for i, n in mods.items()]
    dyn += [(oe.DT_SCE_IMPORT_LIB, (i << 48) | (1 << 32) | s(n)) for i, n in libs.items()]
    dyn += [(oe.DT_SCE_EXPORT_LIB, (i << 48) | (1 << 32) | s(n)) for i, n in export_libs.items()]
    symtab = b"".join(syms)
    dyn += [(oe.DT_SCE_STRTAB, 0), (oe.DT_SCE_STRSZ, len(strtab)), (oe.DT_SCE_SYMTAB, len(strtab)),
            (oe.DT_SCE_SYMTABSZ, len(symtab)), (oe.DT_NULL, 0)]
    dyn_blob = b"".join(struct.pack("<QQ", t, v) for t, v in dyn)
    dyn_off = 0x40 + 2 * 0x38
    data_off = dyn_off + len(dyn_blob)
    data = bytes(strtab) + symtab
    ehdr = b"\x7fELF" + bytes([2, 1, 1, 9]) + bytes(8) + struct.pack(
        "<HHIQQQIHHHHHH", e_type, 0x3E, 1, 0, 0x40, 0, 0, 0x40, 0x38, 2, 0, 0, 0)
    phdrs = struct.pack("<IIQQQQQQ", oe.PT_DYNAMIC, 4, dyn_off, 0, 0, len(dyn_blob), len(dyn_blob), 8)
    phdrs += struct.pack("<IIQQQQQQ", oe.PT_SCE_DYNLIBDATA, 4, data_off, 0, 0, len(data), len(data), 16)
    path.write_bytes(ehdr + phdrs + dyn_blob + data)


NAMES = ["sceKernelOpen", "sceKernelGetpid", "sceAppContentFoo", "sceHostFunc", "sceHostStub",
         "scePrivFunc", "sceGoneFunc", "memcpy", "sceExtThing"]
NO_NAME = NID("notInTheNameDatabase")
BY_NID = NID("implementedByNidOnly")  # no name; the extension matches it by NID

HLE_C = """\
#define LK "libkernel"
static const struct hle_ent g_hle[] = {
    {LK, "sceKernelOpen", hle_open},
    /* {LK, "sceKernelGetpid", hle_getpid}, */
};
"""
BOOT_C = 'static const char *const BUNDLED[] = {\n    "libc.prx", "libSceFios2.prx",\n};\n'
EXT_C = ('#define EX "libSceExt"\nstatic const struct ext_ent g_ext[] = {\n    {"libSceExt", "sceExtThing", ext_thing},\n'
         '    {EX, NULL, ext_unnamed, "%s"}, /* Func_... */\n};\n' % BY_NID)
SYSMODULES = """\
# name size directory
libkernel.sprx\t100\t/system/common/lib
libSceAppContent.sprx\t100\t/system/common/lib
libSceHost.sprx\t100\t/system/common/lib
libScePriv.sprx\t100\t/system/priv/lib
"""
SHAD_CPP = f"""\
int PS4_SYSV_ABI sceHostFunc(int a) {{
    if (a > 1) {{
        return a * 2;
    }}
    return compute(a);
}}

s32 PS4_SYSV_ABI sceHostStub(void* out) {{
    LOG_ERROR(Lib_Host, "(STUBBED) called");
    return ORBIS_OK;
}}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {{
    LIB_FUNCTION("{NID('sceHostFunc')}", "libSceHost", 1, "libSceHost", sceHostFunc);
    LIB_FUNCTION("{NID('sceHostStub')}", "libSceHost", 1,
                 "libSceHost", ORBIS(sceHostStub));
    // LIB_FUNCTION("{NID('sceGoneFunc')}", "libSceGone", 1, "libSceGone", sceGoneFunc);
}}
"""


class ImportReportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        app0 = t / "app0"
        (app0 / "sce_module").mkdir(parents=True)
        mods = {1: "libkernel", 2: "libSceAppContentUtil", 3: "libSceHost", 4: "libScePriv", 5: "libSceGone",
                6: "libc", 7: "libSceExt"}
        libs = {1: "libkernel", 2: "libSceAppContent", 3: "libSceHost", 4: "libScePriv", 5: "libSceGone",
                6: "libc", 7: "libSceExt"}
        func = oe.STT_FUNC
        make_elf(app0 / "eboot.bin", oe.ET_SCE_DYNEXEC, "game",
                 ["libkernel.prx", "libSceAppContent.prx", "libSceHost.prx", "libScePriv.prx", "libSceGone.prx",
                  "libc.prx", "libSceExt.prx"], mods, libs, {},
                 [(NID("sceKernelOpen"), 1, 1, func), (NID("sceKernelGetpid"), 1, 1, func),
                  (NID("sceAppContentFoo"), 2, 2, func), (NID("sceHostFunc"), 3, 3, func),
                  (NID("sceHostStub"), 3, 3, func), (NO_NAME, 3, 3, func), (NID("scePrivFunc"), 4, 4, func),
                  (NID("sceGoneFunc"), 5, 5, func), (NID("memcpy"), 6, 6, func),
                  (NID("sceExtThing"), 7, 7, func), (BY_NID, 7, 7, func)], [])
        make_elf(app0 / "sce_module" / "libc.prx", oe.ET_SCE_DYNAMIC, "libc", [], {}, {}, {1: "libc"}, [],
                 [(NID("memcpy"), 1)])
        native = t / "native"
        (native / "src").mkdir(parents=True)
        (native / "ext").mkdir()
        (native / "src" / "hle.c").write_text(HLE_C)
        (native / "src" / "boot.c").write_text(BOOT_C)
        (native / "ext" / "ext.c").write_text(EXT_C)
        (t / "sysmodules.txt").write_text(SYSMODULES)
        lib_dir = t / "shadPS4" / "src" / "core" / "libraries" / "host"
        lib_dir.mkdir(parents=True)
        (lib_dir / "host.cpp").write_text(SHAD_CPP)
        (t / "names.txt").write_text("\n".join(NAMES) + "\n")
        cls.paths = t, app0, native
        cls.rep = ir.build_report(app0, oe.load_name_db(t / "names.txt"), ir.load_core_hle(native),
                                  ir.load_ext_hle([native / "ext"]), ir.load_sysmodules(t / "sysmodules.txt"),
                                  ir.ShadPS4(t / "shadPS4"), ir.load_bundled(native))
        cls.by_nid = {i["nid"]: i for i in cls.rep["imports"]}

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def imp(self, name: str) -> dict:
        return self.by_nid[NID(name)]

    def test_classes(self) -> None:
        expect = {"sceKernelOpen": "CORE-HLE", "sceExtThing": "EXT-HLE", "memcpy": "TITLE",
                  "sceKernelGetpid": "HOST", "sceAppContentFoo": "HOST", "sceHostFunc": "HOST",
                  "sceHostStub": "HOST", "scePrivFunc": "UNRESOLVED", "sceGoneFunc": "UNRESOLVED"}
        for name, cls in expect.items():
            self.assertEqual(self.imp(name)["cls"], cls, name)
        self.assertEqual(self.by_nid[NO_NAME]["cls"], "UNRESOLVED")
        self.assertIn("no name", self.by_nid[NO_NAME]["reason"])
        # no name, but the extension implements it by NID
        self.assertEqual(self.by_nid[BY_NID]["cls"], "EXT-HLE")
        self.assertEqual(self.rep["summary"]["functions_by_class"],
                         {"CORE-HLE": 1, "EXT-HLE": 2, "TITLE": 1, "HOST": 4, "UNRESOLVED": 3})

    def test_nid_entries(self) -> None:
        src = '#define DM "libSceDiscMap"\nx = {\n    {DM, NULL, f, "fJgP+wqifno"},\n    {"libSceX", NULL, g, "ioKMruft1ek"},\n};\n'
        self.assertEqual(sorted(ir.hle_entries(src, tables_only=False)),
                         [("libSceDiscMap", ir.NID_KEY + "fJgP+wqifno"), ("libSceX", ir.NID_KEY + "ioKMruft1ek")])

    def test_providers(self) -> None:
        self.assertEqual(self.imp("sceKernelOpen")["provider"], "hle.c")
        self.assertEqual(self.imp("memcpy")["provider"], "sce_module/libc.prx")
        self.assertEqual(self.imp("sceKernelGetpid")["provider"], "libkernel.sprx")
        # module libSceAppContentUtil: no such file, found through DT_NEEDED
        self.assertEqual(self.imp("sceAppContentFoo")["provider"], "libSceAppContent.sprx")
        self.assertIn("/system/priv/lib", self.imp("scePrivFunc")["reason"])
        mf = {r["module"]: r for r in self.rep["module_files"]}
        self.assertEqual(mf["libSceAppContentUtil"]["loads"], "libSceAppContent.sprx")
        self.assertEqual([m["loaded_by_boot"] for m in self.rep["modules"]], [None, True])

    def test_shadps4(self) -> None:
        self.assertEqual(self.imp("sceHostFunc")["shadps4"], "impl")
        self.assertEqual(self.imp("sceHostStub")["shadps4"], "stub")
        self.assertIsNone(self.imp("sceGoneFunc")["shadps4"])  # commented out
        host = next(r for r in self.rep["libraries"] if r["library"] == "libSceHost")
        self.assertEqual(host["risk"], "HIGH")  # the unnamed NID
        self.assertEqual((host["shadps4"]["impl"], host["shadps4"]["stub"]), (1, 1))
        self.assertEqual(self.rep["libraries"][-1]["risk"], "LOW")

    def test_homebrew_routes(self) -> None:
        # resolve.c loads module libSceFreeType from libSceFreeTypeOl.sprx (OpenOrbis links "libSceFreeType")
        self.assertEqual(ir.MODULE_FILES.get("libSceFreeType"), "libSceFreeTypeOl")
        sysmods = {"libSceFreeTypeOl.sprx": [ir.SysModule("libSceFreeTypeOl.sprx", 1, "/system/common/lib")]}
        self.assertEqual(ir.host_file("libSceFreeType", ["libSceFreeType.prx"], sysmods)[0], "libSceFreeTypeOl.sprx")
        # budget.c's plan-free memory calls: anonymous mmap is the title's flexible memory
        core = ir.load_core_hle(Path(ir.__file__).resolve().parents[1] / "native")
        self.assertIn(("libkernel", "mmap"), core)
        self.assertIn(("libkernel", "munmap"), core)

    def test_body_classifier(self) -> None:
        self.assertEqual(ir.classify_body('{\n    LOG_ERROR(Lib_X, "(STUBBED) called");\n'
                                          '    if (!out) {\n        return -1;\n    }\n    *out = 0;\n'
                                          '    return ORBIS_OK;\n}'), "stub")
        self.assertEqual(ir.classify_body("{\n    switch (x) {\n    case 0: a(); b(); c();\n"
                                          "    default: UNREACHABLE();\n    }\n}"), "impl")
        self.assertEqual(ir.classify_body('{\n    LOG_INFO(Lib_X, "(PARTIAL) called");\n    return 0;\n}'),
                         "partial")
        self.assertEqual(ir.strip_comments('a("//x"); // c\n/* b\n */ d;'), 'a("//x"); \n\n d;')

    def test_outputs(self) -> None:
        t, app0, native = self.paths
        out = t / "out"
        with redirect_stderr(io.StringIO()):
            ir.main([str(app0), "--names", str(t / "names.txt"), "--native", str(native),
                     "--sysmodules", str(t / "sysmodules.txt"), "--shadps4", str(t / "shadPS4"),
                     "--md", str(out / "r.md"), "--json", str(out / "r.json")])
        md = (out / "r.md").read_text()
        self.assertIn("## UNRESOLVED (3)", md)
        self.assertIn("| HIGH | libSceGone |", md)
        self.assertIn("libSceGone | libSceGone | 1 | 0 |", md)  # missing host modules table
        self.assertEqual(json.loads((out / "r.json").read_text())["summary"]["functions"], 11)  # 10 + the one matched by NID


if __name__ == "__main__":
    unittest.main()
