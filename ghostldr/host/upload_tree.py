#!/usr/bin/env python3
"""Upload a directory tree to the PS5 over ftpsrv with parallel connections.

Resumable: files whose remote size already matches are skipped.
    python3 ghostldr/host/upload_tree.py build/ghost/app0 /data/ghost/app0
"""

from __future__ import annotations

import argparse
import ftplib
import os
import queue
import threading
import time
from pathlib import Path

from console import console_ip


def connect(host: str, port: int) -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(host, port, timeout=60)
    f.login()
    f.voidcmd("TYPE I")
    return f


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("dst")
    ap.add_argument("--host", default=None, help="console address (default: looked up by host-id)")
    ap.add_argument("--port", type=int, default=2121)
    ap.add_argument("-j", "--jobs", type=int, default=8)
    args = ap.parse_args()
    args.host = args.host or console_ip()

    files = sorted(p for p in args.src.rglob("*") if p.is_file() and not p.name.endswith(".part"))
    total = sum(p.stat().st_size for p in files)
    dirs = sorted({str(Path(args.dst) / p.parent.relative_to(args.src)) for p in files} | {args.dst})

    ftp = connect(args.host, args.port)
    made = set()
    for d in dirs:
        parts = d.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            sub = "/" + "/".join(parts[:i])
            if sub in made:
                continue
            try:
                ftp.mkd(sub)
            except ftplib.error_perm:
                pass
            made.add(sub)
    ftp.close()

    work: queue.Queue[Path] = queue.Queue()
    for p in sorted(files, key=lambda p: -p.stat().st_size):
        work.put(p)
    lock = threading.Lock()
    stats = {"bytes": 0, "skipped": 0, "done": 0, "errors": 0}
    t0 = time.time()

    def worker() -> None:
        ftp = connect(args.host, args.port)
        while True:
            try:
                p = work.get_nowait()
            except queue.Empty:
                break
            remote = f"{args.dst}/{p.relative_to(args.src).as_posix()}"
            size = p.stat().st_size
            for attempt in range(3):
                try:
                    try:
                        if ftp.size(remote) == size:
                            with lock:
                                stats["skipped"] += 1
                                stats["bytes"] += size
                            break
                    except ftplib.error_perm:
                        pass
                    with p.open("rb") as fh:
                        ftp.storbinary(f"STOR {remote}", fh, blocksize=1 << 20)
                    with lock:
                        stats["done"] += 1
                        stats["bytes"] += size
                    break
                except (OSError, ftplib.Error) as e:
                    try:
                        ftp.close()
                    except Exception:
                        pass
                    time.sleep(1)
                    ftp = connect(args.host, args.port)
                    if attempt == 2:
                        with lock:
                            stats["errors"] += 1
                        print(f"ERROR {remote}: {e}", flush=True)
        ftp.close()

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(args.jobs)]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(10)
        with lock:
            el = time.time() - t0
            print(f"{stats['bytes'] / 2**30:.1f}/{total / 2**30:.1f} GiB "
                  f"{stats['bytes'] / max(el, 1) / 2**20:.0f} MiB/s files {stats['done']}+{stats['skipped']}"
                  f"/{len(files)} err {stats['errors']}", flush=True)
    print("upload finished", stats, flush=True)
    return 1 if stats["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
