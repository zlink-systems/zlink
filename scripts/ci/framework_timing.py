"""Write UTC timestamps and elapsed seconds for Framework build steps."""

from contextlib import contextmanager
from datetime import datetime, timezone
import subprocess
import sys
import time


def event(label, state, started=None):
    timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
    elapsed = "" if started is None else f" elapsed_seconds={time.monotonic() - started:.3f}"
    print(f"[timing] {timestamp} {label} {state}{elapsed}", file=sys.stderr, flush=True)


@contextmanager
def timed(label):
    started = time.monotonic()
    event(label, "start")
    try:
        yield
    finally:
        event(label, "end", started)


if __name__ == "__main__":
    label, *command = sys.argv[1:]
    with timed(label):
        sys.exit(subprocess.call(command))
