# The selector's built-in font

`notosans.glf` holds the outlines of **Noto Sans Regular** that the game selector draws text with when the console's system font can't be opened (docs/selector.md). It's in the selector's own compact format (GLF1, described in `ghostldr/tools/mkfont.py`). It covers Latin, Greek, Cyrillic and common punctuation, about 600 code points, plus the font's kerning between printable ASCII characters.

- **Font:** Noto Sans Regular, Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic).
- **License:** SIL Open Font License 1.1, in `OFL.txt` next to this file. It covers `notosans.glf` and the copy of it inside `build/ghostldr/selector.bin`, so a release that ships `selector.bin` ships `OFL.txt` with it. The font has no Reserved Font Name.
- **Source file:** `fonts/NotoSans/unhinted/ttf/NotoSans-Regular.ttf` from https://github.com/notofonts/notofonts.github.io, SHA-256 `f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8` (431,364 bytes).

To regenerate it:

```bash
python3 ghostldr/tools/mkfont.py NotoSans-Regular.ttf ghostldr/native/selector/font/notosans.glf
```

No font, image or sound from the console's system software is in this repository. The selector loads the system font (SST) only at run time, on the console, through libSceFont.
