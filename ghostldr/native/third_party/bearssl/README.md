# BearSSL 0.6

The TLS library the game selector's plan sync uses (`ghostldr/native/plansync`, docs/community-plans.md): `inc/`, `src/` and `LICENSE.txt` (MIT) from https://www.bearssl.org/bearssl-0.6.tar.gz (SHA-256 6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14), unchanged.

`selector/selector.mk` compiles all of `src/` into an archive with `BR_RDRAND`, `BR_USE_URANDOM`, `BR_USE_UNIX_TIME`, `BR_AES_X86NI`, `BR_SSE2` and `BR_POWER8` off (no CPU feature tests, no system clock or random source: `plansync.c` gives it the console's), so only what `plansync.c` calls is linked into the selector.
