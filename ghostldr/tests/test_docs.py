"""The documentation's links, offline: every relative link in README.md,
docs/*.md and the READMEs under ghostldr/ points at a file that exists, and
a link to a heading (`file.md#heading`) at a heading that file has, by
GitHub's rules for heading anchors.  Also: the release guide needs no Python.

    python3 -m unittest discover -s ghostldr/tests
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r"\]\(([^)\s]+)\)")


def doc_files() -> list[Path]:
    return [ROOT / "README.md"] + sorted((ROOT / "docs").glob("*.md")) + sorted((ROOT / "ghostldr").rglob("README.md"))


def slug(heading: str) -> str:
    """GitHub's anchor for a heading: link text kept, lower case, punctuation
    other than '-' and '_' dropped, spaces to '-'."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading).replace("`", "").strip().lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def without_code(text: str) -> str:
    return re.sub(r"```.*?```", "", text, flags=re.S)


def anchors(path: Path) -> set[str]:
    out: set[str] = set()
    seen: dict[str, int] = {}
    for line in without_code(path.read_text(encoding="utf-8")).splitlines():
        m = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
        if m:
            s = slug(m.group(1))
            n = seen.get(s, 0)
            seen[s] = n + 1
            out.add(s if n == 0 else f"{s}-{n}")
    return out


class Links(unittest.TestCase):
    def test_slug(self) -> None:
        self.assertEqual(slug("Every later session: ghostldr.lua alone"), "every-later-session-ghostldrlua-alone")
        self.assertEqual(slug("4. Boot"), "4-boot")
        self.assertEqual(slug("Save tools (Apollo Save Tool)"), "save-tools-apollo-save-tool")
        self.assertEqual(slug("`tools/stub_report.py`"), "toolsstub_reportpy")

    def test_every_relative_link_resolves(self) -> None:
        bad = []
        for f in doc_files():
            for m in LINK.finditer(without_code(f.read_text(encoding="utf-8"))):
                target = m.group(1)
                if re.match(r"^[a-z][a-z0-9+.-]*:", target):
                    continue  # https:, mailto:, ...
                path, _, frag = target.partition("#")
                path = path.split("?")[0]
                dest = (f.parent / path).resolve() if path else f
                where = f"{f.relative_to(ROOT)}: {target}"
                if not dest.exists():
                    bad.append(f"{where}: no such file")
                elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                    bad.append(f"{where}: no such heading")
        self.assertEqual(bad, [], "\n".join(bad))


class ReleaseGuide(unittest.TestCase):
    def test_no_python_but_for_developers(self) -> None:
        text = (ROOT / "docs/using-the-release.md").read_text(encoding="utf-8")
        main, _, developers = text.partition("## For developers")
        self.assertTrue(developers, "a For developers section")
        for word in ("python3", "ghostldr.py", "run.py", "gfs_install"):
            self.assertFalse(word in main, f"{word} before For developers")
        self.assertFalse("app/" in text, "a link to the removed desktop app")

    def test_the_release_files_are_named(self) -> None:
        text = (ROOT / "docs/using-the-release.md").read_text(encoding="utf-8")
        for name in ("ghostldr.lua", "ghostldr-ext.bin", "ghostldr-elfldr.bin", "manager.lua", "README.txt",
                     "SHA256SUMS.txt", "ghostldr-<version>.zip"):
            self.assertIn(name, text)


if __name__ == "__main__":
    unittest.main()
