# Community memory plans: collection server and daily job

Players who opt in share the memory plans their consoles learned; the
console's game selector sends them to the collection server (`server.py`),
and a daily job (`sync_plans.py`) pulls what came in, picks one plan per
eboot fingerprint and platform and writes `plans/` in this repository. The
formats, the API and the `plans/` layout are in
[docs/community-plans.md](../../docs/community-plans.md).

Both are Python 3.11, standard library only. Tests:
`python3 -m unittest discover -s ghostldr/tests -p test_contrib.py`.

## The server

Any small Linux host with Python 3.11 will do (a Debian container is
plenty). It listens on one port, `0.0.0.0:8790` by default, and answers
`GET /v1/health`, `POST /v1/plans` and `GET /v1/submissions`.

    useradd --system --home /var/lib/ghostldr-plans ghostldr
    install -d -o ghostldr /var/lib/ghostldr-plans /etc/ghostldr-plans
    install -m 644 server.py /opt/ghostldr-plans/server.py
    sh -c 'umask 077; for k in contrib admin; do
        python3 -c "import secrets; print(secrets.token_hex(32))" > /etc/ghostldr-plans/$k.key; done'
    chown ghostldr /etc/ghostldr-plans/*.key

and a systemd unit running, as user `ghostldr` with `Restart=always`:

    python3 /opt/ghostldr-plans/server.py --db /var/lib/ghostldr-plans/plans.db \
        --contrib-key-file /etc/ghostldr-plans/contrib.key --admin-key-file /etc/ghostldr-plans/admin.key

It logs one line per request, never a key or a plan. Consoles reach it over
HTTPS on a public hostname: put it behind a reverse proxy or a tunnel (a
Cloudflare tunnel's public hostname pointed at `http://<host>:8790` works).
The server believes `CF-Connecting-IP` for its 30-an-hour limit when the
connection comes from a private address (`--trusted-proxies`).

## Keys

The contribution key only adds plans. The build puts it, and the public
address, into the game selector (`GHOSTLDR_CONTRIB_KEY` and
`GHOSTLDR_CONTRIB_URL`, else `~/.config/ghostldr/contrib.key` and
`contrib-url.txt` on the machine that builds the release; never in git). The
admin key reads submissions and stays with the daily job
(`~/.config/ghostldr/contrib-admin.key`).

Rotating the contribution key: write a new one on the server, restart it,
copy it to the release machine's `~/.config/ghostldr/contrib.key`, and build
the next release. Older releases stop sharing until they are replaced, so
rotate it only when it has leaked.

## The daily job

Once a day, in the ghostldr checkout:

    python3 tools/contrib/sync_plans.py          # --dry-run to see first
    if [ -n "$(git status --porcelain plans)" ]; then
        git add plans && git commit -m "Plans: ..." && git push
    fi

`sync_plans.py` reads the admin key from `~/.config/ghostldr/contrib-admin.key`
and the server from `--server`, else `GHOSTLDR_CONTRIB_SERVER` or
`~/.config/ghostldr/contrib-server.txt` (its address on the job's own
network), else the consoles' `GHOSTLDR_CONTRIB_URL` or `contrib-url.txt`. It
remembers the last submission it saw and every install's latest plan in
`~/.config/ghostldr/contrib-sync.json`, so losing that file means the next run
starts from submission 0 (which is fine: the server still holds every
install's latest plan). For each fingerprint and platform it picks a plan
that did not run out of memory and stayed up at least 2 minutes, then the
most boots, then the latest, and rewrites only the files that changed. With
nothing new it prints `no new submissions` and exits 0 without touching
`plans/`. A server it can't reach, or a refused key, exits 1.
