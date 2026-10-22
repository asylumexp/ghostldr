"""Background jobs: the host scripts run as child processes.

A job is one script run.  Its output (stdout and stderr together) is kept as
numbered lines that the UI reads with a cursor, waiting for new ones (long
polling).  Jobs declare the resources they use ("console": Luac0re's loader
and the PS5; "pack": build/ghostfs); two jobs that share one never run at
the same time, so only one job talks to the console at once.

Cancelling asks the script to stop the way Ctrl+C would (SIGINT to its
process group; CTRL_BREAK on Windows), then terminates it if it hasn't
stopped after a grace period.
"""

from __future__ import annotations

import itertools
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable

from . import problems

MAX_LINES = 5000                 # kept in memory per job (the full boot log is in run.log)
LOG_FILE_LIMIT = 5 << 20         # bytes of each job's output kept on disk
GRACE = 6.0                      # seconds between asking nicely and terminating


class Busy(Exception):
    """A job that needs the same resource is running."""

    def __init__(self, job: "Job") -> None:
        super().__init__(f"{job.label} is still running")
        self.job = job


class Job:
    def __init__(self, jid: str, kind: str, label: str, argv: list[str], locks: set[str],
                 tracker=None, meta: dict | None = None) -> None:
        self.id, self.kind, self.label, self.argv, self.locks = jid, kind, label, argv, set(locks)
        self.tracker = tracker
        self.meta = dict(meta or {})
        self.state = "starting"           # running, done, failed, cancelled
        self.rc: int | None = None
        self.started = time.time()
        self.ended: float | None = None
        self.lines: list[str] = []
        self.first_seq = 0                # sequence number of lines[0]
        self.next_seq = 0
        self.cancel_requested = False
        self.problem: dict | None = None
        self.error = ""
        self.proc: subprocess.Popen | None = None
        self.cond = threading.Condition()
        self.logfile = None
        self.log_bytes = 0

    @property
    def running(self) -> bool:
        return self.state in ("starting", "running")

    def add_line(self, line: str) -> None:
        with self.cond:
            self.lines.append(line)
            self.next_seq += 1
            if len(self.lines) > MAX_LINES:
                drop = len(self.lines) - MAX_LINES
                del self.lines[:drop]
                self.first_seq += drop
            if self.tracker:
                try:
                    self.tracker.feed(line)
                except Exception as e:   # a parser bug must not stop the job
                    self.tracker_error = f"{type(e).__name__}: {e}"
            self.cond.notify_all()
        if self.logfile and self.log_bytes < LOG_FILE_LIMIT:
            data = (line + "\n").encode("utf-8", "replace")
            self.log_bytes += len(data)
            try:
                self.logfile.write(data)
                self.logfile.flush()
            except OSError:
                pass

    def summary(self, progress: bool = True) -> dict:
        """What the UI shows about the job; `progress` adds its tracker's
        snapshot (left out of lists: a report's can be large)."""
        out = {"id": self.id, "kind": self.kind, "label": self.label, "state": self.state, "rc": self.rc,
               "started": self.started, "ended": self.ended, "lines": self.next_seq, "meta": self.meta,
               "problem": self.problem, "error": self.error, "cancel_requested": self.cancel_requested}
        if progress:
            out["progress"] = self.tracker.snapshot() if self.tracker else None
        return out

    def read(self, since: int = 0, wait: float = 0.0, limit: int = 2000) -> dict:
        """Lines from `since` on (waiting up to `wait` s for new ones while the job runs)."""
        deadline = time.time() + max(0.0, min(wait, 30.0))
        with self.cond:
            while self.next_seq <= since and self.running and time.time() < deadline:
                self.cond.wait(max(0.05, deadline - time.time()))
            start = max(since, self.first_seq)
            lines = self.lines[start - self.first_seq:start - self.first_seq + limit]
            out = self.summary()
            out.update(since=start, next=start + len(lines), output=lines, truncated=since < self.first_seq)
            return out


class JobManager:
    def __init__(self, cwd: Path, log_dir: Path | None = None, keep: int = 40) -> None:
        self.cwd = cwd
        self.log_dir = log_dir
        self.keep = keep
        self.jobs: dict[str, Job] = {}
        self.order: list[str] = []
        self.lock = threading.Lock()
        self.ids = itertools.count(1)
        self.on_finish: list[Callable[[Job], None]] = []
        self.idle = threading.Event()
        self.idle.set()

    # ---- starting ----

    def holder(self, locks: set[str]) -> Job | None:
        """The running job that holds one of `locks`, if any."""
        for j in self.jobs.values():
            if j.running and j.locks & locks:
                return j
        return None

    def start(self, kind: str, label: str, argv: list[str], locks: set[str] = frozenset(), tracker=None,
              env: dict | None = None, meta: dict | None = None) -> Job:
        with self.lock:
            busy = self.holder(set(locks))
            if busy:
                raise Busy(busy)
            jid = f"{int(time.time())}-{next(self.ids)}"
            job = Job(jid, kind, label, argv, set(locks), tracker, meta)
            self.jobs[jid] = job
            self.order.append(jid)
            self._prune()
            self.idle.clear()
        self._open_log(job)
        try:
            job.proc = subprocess.Popen(argv, cwd=str(self.cwd), env=env, **spawn_options())
        except OSError as e:
            job.error = f"could not start {Path(argv[1]).name if len(argv) > 1 else argv[0]}: {e}"
            job.add_line(job.error)
            self._finish(job, -1)
            return job
        job.state = "running"
        threading.Thread(target=self._pump, args=(job,), daemon=True, name=f"job-{jid}").start()
        return job

    def _open_log(self, job: Job) -> None:
        if not self.log_dir:
            return
        try:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(job.started))
            job.logfile = (self.log_dir / f"{stamp}-{job.kind}.log").open("ab")
            job.logfile.write(f"# {job.label}\n# {' '.join(job.argv[1:])}\n".encode("utf-8", "replace"))
            logs = sorted(self.log_dir.glob("*.log"))
            for old in logs[:-self.keep]:
                old.unlink(missing_ok=True)
        except OSError:
            job.logfile = None

    def _pump(self, job: Job) -> None:
        assert job.proc and job.proc.stdout
        pending = b""
        while True:
            chunk = job.proc.stdout.read1(65536) if hasattr(job.proc.stdout, "read1") else job.proc.stdout.read(4096)
            if not chunk:
                break
            pending += chunk
            tail = b"\r" if pending.endswith(b"\r") else b""     # maybe half of a \r\n
            head = pending[:-1] if tail else pending
            *whole, rest = head.replace(b"\r\n", b"\n").replace(b"\r", b"\n").split(b"\n")
            pending = rest + tail
            for raw in whole:
                job.add_line(raw.decode("utf-8", "replace"))
        if pending:
            job.add_line(pending.decode("utf-8", "replace"))
        job.proc.stdout.close()
        rc = job.proc.wait()
        self._finish(job, rc)

    def _finish(self, job: Job, rc: int) -> None:
        with job.cond:
            job.rc = rc
            job.ended = time.time()
            if job.cancel_requested:
                job.state = "cancelled"
            elif rc == 0:
                job.state = "done"
            else:
                job.state = "failed"
                job.problem = problems.diagnose(job.lines)
                job.error = job.error or problems.last_error(job.lines)
            if job.tracker:
                try:
                    job.tracker.finish(job)
                except Exception as e:
                    job.error = job.error or f"reading the result failed: {e}"
            job.cond.notify_all()
        if job.logfile:
            try:
                job.logfile.write(f"# exit {rc} ({job.state})\n".encode())
                job.logfile.close()
            except OSError:
                pass
        for cb in list(self.on_finish):
            try:
                cb(job)
            except Exception:
                pass
        with self.lock:
            if not any(j.running for j in self.jobs.values()):
                self.idle.set()

    def _prune(self) -> None:
        while len(self.order) > self.keep:
            old = self.jobs.get(self.order[0])
            if old and old.running:
                break
            self.jobs.pop(self.order.pop(0), None)

    # ---- looking and stopping ----

    def get(self, jid: str) -> Job | None:
        return self.jobs.get(jid)

    def running(self) -> list[Job]:
        return [j for j in self.jobs.values() if j.running]

    def latest(self, kind: str) -> Job | None:
        for jid in reversed(self.order):
            j = self.jobs.get(jid)
            if j and j.kind == kind:
                return j
        return None

    def list(self) -> list[dict]:
        return [self.jobs[j].summary(progress=False) for j in reversed(self.order) if j in self.jobs]

    def cancel(self, jid: str) -> bool:
        job = self.jobs.get(jid)
        if not job or not job.running or not job.proc:
            return False
        job.cancel_requested = True
        threading.Thread(target=stop_process, args=(job.proc,), daemon=True).start()
        return True

    def cancel_all(self, wait: float = GRACE + 2) -> None:
        for j in self.running():
            self.cancel(j.id)
        self.idle.wait(wait)


def spawn_options() -> dict:
    """Pipes, no stdin, and a process group of its own (so a cancel reaches
    the script and anything it started, never this server)."""
    opts: dict = {"stdin": subprocess.DEVNULL, "stdout": subprocess.PIPE, "stderr": subprocess.STDOUT}
    if os.name == "nt":
        flags = subprocess.CREATE_NEW_PROCESS_GROUP
        if not has_console():
            flags |= subprocess.CREATE_NO_WINDOW
        opts["creationflags"] = flags
    else:
        opts["start_new_session"] = True
    return opts


def has_console() -> bool:
    if os.name != "nt":
        return True
    try:
        import ctypes
        return bool(ctypes.windll.kernel32.GetConsoleWindow())
    except Exception:
        return False


def stop_process(proc: subprocess.Popen, grace: float = GRACE) -> None:
    """Ctrl+C, then terminate, then kill."""
    if proc.poll() is not None:
        return
    try:
        if os.name == "nt":
            proc.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            os.killpg(proc.pid, signal.SIGINT)
    except (OSError, ValueError):
        pass
    try:
        proc.wait(grace)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        if os.name == "nt":
            proc.terminate()
        else:
            os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(3)
        return
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        if os.name == "nt":
            proc.kill()
        else:
            os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass


def python_argv(script: Path, *args: str) -> list[str]:
    """This Python, unbuffered, running `script`."""
    return [sys.executable, "-u", str(script), *args]
