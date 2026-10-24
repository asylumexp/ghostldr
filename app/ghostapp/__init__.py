"""ghostldr desktop app: a local web UI over the host tools in ghostldr/host.

The backend is the Python standard library only (http.server, threads,
subprocess).  It runs the existing host scripts as background jobs and reads
their output; it never reimplements what they do.  See app/README.md.
"""

APP_VERSION = "0.6.4"
