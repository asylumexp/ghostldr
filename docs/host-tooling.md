# Host tooling

Four host-side additions to the release flow. All are read-only with
respect to the console except where noted.

## `run.py doctor`

    python3 ghostldr/host/run.py doctor [--host <console-ip>]

Checks the whole boot chain from this computer and prints one
PASS / WARN / FAIL line per item: Python version, prebuilt core
integrity (`core.bin` size vs `core.json`), Lua payload size against
Luac0re's 500 KB cap, whether UDP 18194 / TCP 18195 are free (or
already owned by the recorder), whether the console's Lua loader
answers on 9026, and — when a dump and a name table are staged —
the title's import coverage summary. Exit code 1 if anything FAILs.
Doctor sends nothing to the console beyond a TCP connect probe.

## Boot preflight gate

`run.py boot` now runs `tools/import_report.py` over a staged
`build/ghost/app0` first (only when `build/nid/known_names.txt`
also exists; on trees that cannot run the checker it prints
"skipped" and boots as before). The summary line prints how the
title's functions bind — core-HLE, ext-HLE, title, host — and how
many are UNRESOLVED. Unresolved imports bind to stubs that return 0,
which looks like success to the title, so any unresolved count asks
`Boot anyway? [y/N]` before going on (anything but `y` cancels; with
no terminal to ask on, the boot stops). The full report is written to
`build/ghostldr/preflight.md` (and `.json`). To boot without being
asked:

    python3 ghostldr/host/run.py boot --force-preflight

Two limits for now. The check always looks at `build/ghost/app0`,
whichever title `--title` boots from the save containers. And
`import_report.py` doesn't read extension entries keyed by NID alone
(functions with no known name), so it counts them as unresolved even
though the extension implements them: Ghost of Tsushima and Bloodborne
each import one (`libSceDiscMap`), so they always ask. A release tree
ships no name table, so there the check is skipped.

## `tools/stub_report.py`

    python3 ghostldr/tools/stub_report.py build/ghostldr/run.log [--top N] [--json out.json]

Aggregates the stub lines the core logs at runtime (`UNRESOLVED
call: ...`, `DENIED call: ...`) into a ranked per-function hit
table: totals, distinct stubs, denied hits, and the anonymous
`UNRESOLVED call #<idx>` count. Use it after a session to decide
which HLE functions are worth implementing next — by what the
title actually called, not by what it imports.

## Log hints

When the core's extension fetch/start fails, it logs a terse
`ext: ...` line and the boot stops. run.py's log listener now
follows such lines with a `[hint]` line giving the plain-language
cause and the fix (firewall on TCP 18195, core/ext build mismatch,
corrupt blob, and so on).
