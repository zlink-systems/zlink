"""Run a built pubsub_recv_sample repeatedly on Linux.

Usage: python3 pubsub_recv_sample_stress.py SAMPLE [--count N] [--idle]
Set LD_LIBRARY_PATH to the Core runtime used to build SAMPLE.
"""

import argparse
import subprocess
import time
from pathlib import Path


def run_sample(sample, count, stress):
    failures = 0
    for attempt in range(1, count + 1):
        if stress is not None and stress.poll() is not None:
            raise RuntimeError("CPU load ended before the sample trials finished")
        try:
            result = subprocess.run(
                [sample], capture_output=True, text=True, timeout=30, check=False
            )
            failed = result.returncode != 0
            output = f"exit={result.returncode} {result.stdout}{result.stderr}".rstrip()
        except subprocess.TimeoutExpired:
            failed = True
            output = "timed out after 30 seconds"
        failures += failed
        print(f"{attempt:03} {output}", flush=True)
    print(f"failures={failures}/{count}", flush=True)
    return int(failures != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample", type=Path)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--idle", action="store_true")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be positive")
    sample = args.sample.resolve(strict=True)
    if args.idle:
        return run_sample(sample, args.count, None)

    stress = subprocess.Popen(
        ["nice", "-n", "19", "stress-ng", "--cpu", "12", "--timeout", "300s"]
    )
    workers = []
    try:
        time.sleep(1)
        children = Path(f"/proc/{stress.pid}/task/{stress.pid}/children")
        workers = [int(pid) for pid in children.read_text().split()]
        if len(workers) != 12:
            raise RuntimeError(f"expected 12 CPU workers, got {workers}")
        print(f"stress PID {stress.pid}, workers {workers}", flush=True)
        return run_sample(sample, args.count, stress)
    finally:
        stress.terminate()
        stress.wait()
        remaining = [pid for pid in [stress.pid, *workers] if Path(f"/proc/{pid}").exists()]
        print(f"remaining stress PIDs={remaining}", flush=True)
        if remaining:
            raise RuntimeError(f"CPU load processes still running: {remaining}")


if __name__ == "__main__":
    raise SystemExit(main())
