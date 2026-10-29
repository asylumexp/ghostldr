# Community memory plans

A game without a memory plan of its own learns one on the PS5 ([compatibility.md](compatibility.md#memory-learned-at-run-time)). Players who opt in share those learned plans; a daily job collects them into this repository's `plans/` folder; the game selector downloads the ones for the games on the console, and a console with no plan of its own for that build starts from the community's instead of from nothing. It keeps learning from there, so a community plan is a starting point, never a fixed setting.

The console does it all itself, while the game selector shows: it checks GitHub and sends what it learned, over HTTPS, with no computer involved. Anything that goes wrong (no network, GitHub's rate limit, a bad certificate) is silent: a game whose plan couldn't be checked this time just isn't offered one.

## On the console

**Settings → Advanced** has two switches:

- **Share learned memory plans** (off): each time a game's learned plan is saved, a copy goes to ghostldr's outbox, and the next game selector sends it to the collection server.
- **Use community memory plans** (on): the selector checks GitHub for plans for the games it lists, and a game with no plan of its own for its build starts from the community's.

A game's **Information** panel (OPTIONS) shows *Community plan from N consoles* when there is one for it, and Square turns it off (or on again) for that game alone. A game with none shows nothing about it.

What the log says:

```
plans: 16 stream and 4 datagram sockets for sharing and GitHub's plans (9 games)
plans: shared CUSA17419
plans: GitHub's index: plans for the games here: 2
plans: 2 community plans for the games here (GitHub: HTTP 200); 1 shared, 0 waiting
automem: no plan of its own for this build: starting from the community plan (from 4 consoles, 3 boots)
```

and when GitHub doesn't answer: `plans: GitHub's index: no answer this time (HTTP 403` (the rate limit: 60 requests an hour per address, so a household of consoles can run out; an unchanged index costs nothing, as its ETag is kept).

## The pieces

| Piece | Where | What it does |
|---|---|---|
| Learned plan | `automem.c` | `ghostldr-memory.bin` beside the game's saves (`struct am_plan`, 1080 bytes). |
| Outbox | `automem.c`, `savedata.c` | With `CTX_FLAG_SHARE_MEM`, every save of the plan is also written to ghostldr's `GLDRPLANS` container as `share-<title>-<fingerprint>-<ps5\|ps4>.bin`. |
| Plan sync | `plansync/` in the game selector | On a thread while the games show: sends the outbox to the collection server and removes what it took (or refused for good: 400, 401, 413); then fetches `plans/index.json` (GitHub's API, with the kept ETag) and the plans it lists for the games here (`raw.githubusercontent.com`), each checked against its SHA-256, and keeps them in `GLDRPLANS`. |
| HTTPS | `plansync.c`, `third_party/bearssl` | TLS 1.2 (BearSSL 0.6, MIT): ECDHE with AES-GCM or ChaCha20-Poly1305, certificates checked against `plansync/roots.pem` (17 authorities: ISRG, Sectigo/USERTrust, DigiCert, Google Trust Services, Amazon, SSL.com) and the console's clock. DNS over UDP to the network's own servers. Sockets from the JIT compiler process, made on the boot thread (`plansync_sel.c`). |
| The choice | `selector/src/conf.c`, `ui.c` | The two switches, the Information row, `community_off=` in `GLDRCONF/settings.txt` for the games turned off one by one. |
| Starting from a community plan | `automem.c`, `profile.c` | With no plan of its own for the build and `CTX_FLAG_NO_COMMUNITY_MEM` clear: a plan the selector downloaded this session (`ghost_api.seed_add`), else a `REC_SEED` record in the title's profile (plans in the release, or from `run.py`), becomes last boot's plan. |
| Collection server | `tools/contrib/server.py` | Keeps the latest plan per install, build and platform ([tools/contrib/README.md](../tools/contrib/README.md)). |
| Daily job | `tools/contrib/sync_plans.py` | Pulls new submissions, picks one plan per build and platform, writes `plans/`, commits. |
| Computer side | `host/plans.py`, `profiles.py` | `run.py` checks GitHub the same way (`build/plans/`) and sends the plans as `REC_SEED`, for boots without the selector; the release's `ghostldr.lua` carries the plans that were in `plans/` when it was built. |

The selector module is placed in the emulator's own JIT buffer and is gone before the game loads, so none of this takes the game's memory or the console's scarce new JIT memory. The TLS code needs relocations (pointers in BearSSL's tables): `mksel.py` puts them after the image and `sel.c` applies them before placing it.

## Formats

### `struct am_plan` (automem.c, `AM_VERSION` 1)

Little-endian, 1080 bytes: `u64 magic` "GLDRMEM1" (0x314d454d52444c47), `u32 version` (1), `u32 size` (1080), `u8 fingerprint[20]` (the eboot's), then u32 at 36: `boots, phys_mib, told_mib, peak_mib, demand_mib, oom` (56: bit 0 ran out of what it was told, bit 1 of real memory) `, short_mib, left_mib, alive_min` (68) `, strict, nsites, nshrink`, 32 sites of 16 bytes at 84, 8 shrinks of 60 bytes at 596.

### `GLDRPLANS` (ghostldr's save container, 3 MiB)

`share-*.bin` (the outbox), `plan-<fingerprint>-<platform>.bin` (downloaded plans), `plans-index.json` and `plans-etag.txt` (GitHub's index and its ETag), `install-id.txt` (a random ID, so the server keeps one plan per console and build).

### `REC_SEED` (profile record 9)

`u8 fingerprint[20]`, `u32 platform` (0 PS5, 1 PS4), `u32 contributors`, `u32 plan_size`, `u32 reserved`, then `plan_size` bytes of `struct am_plan`. A title's profile blob may carry several (one per build and platform). `profiles.py` adds them from `plans/` (the release's) and, for `run.py`, `build/plans/`; a title with plans but no profile gets a generic blob with only its seeds.

### `plans/` in this repository

```
plans/index.json
plans/CUSA17419/0ac6bc17db533c799dcccaa4fcf85ebf00000000-ps5.bin
```

`index.json`: `{"version": 1, "plans": [{"title", "fingerprint", "platform", "path", "sha256", "contributors", "boots", "updated"}]}`.

### Collection server API

- `POST /v1/plans`, header `Authorization: Bearer <contribution key>`, JSON `{"install": "<random 32 hex>", "title", "platform", "fingerprint", "plan": "<base64>", "ghostldr": "<version>"}`. 201 stored, 200 unchanged, 400 invalid, 401 bad key, 413 too big, 429 too many (30 an hour per address).
- `GET /v1/submissions?after=<id>`, header `Authorization: Bearer <admin key>`: `{"submissions": [{"id", "received", "install", "title", "platform", "fingerprint", "plan", "ghostldr"}], "last": <id>}`.
- `GET /v1/health`: `{"ok": true}`.

The server's address and the contribution key are built into the selector from the build machine (`GHOSTLDR_CONTRIB_URL` and `GHOSTLDR_CONTRIB_KEY`, else `~/.config/ghostldr/contrib-url.txt` and `contrib.key`; the Makefile writes them into `build/ghostldr/sel/contrib.h`), never into git. The key only lets a client add plans. A build without them keeps the outbox for one that has them. The admin key stays on the machine that runs the daily job.

### ctx flags

| Flag | Bit | Set by |
|---|---|---|
| `CTX_FLAG_SHARE_MEM` | 27 (134217728) | Settings switch `share_memory` (on sets it), `run.py --share-memory` |
| `CTX_FLAG_NO_COMMUNITY_MEM` | 28 (268435456) | Settings switch `community_memory` (off sets it), the game's own choice in its Information panel, `run.py --no-community-memory` |

## Tests

- `test_plansync.py`: `plansync.c` built for the computer against fake GitHub and collection servers (the index with its ETag, a 304, the rate limit, a plan whose sum is wrong, the outbox kept or emptied); `GHOSTLDR_LIVE_TLS=1` also checks the handshakes with GitHub's real hosts.
- `test_profiles.py`: a build without a plan starts from a community plan and learns on; the switch off and other builds don't; the outbox is written only with the share switch.
- `test_selector.py`: the switches, the Information row and the flag at start.
- `test_contrib.py`: the collection server and the daily job's choice.
