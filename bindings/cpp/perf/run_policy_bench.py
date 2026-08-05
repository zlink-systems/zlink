#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path


def main() -> int:
    perf_dir = Path(__file__).resolve().parent
    args = sys.argv[1:]
    binding = None
    suite = None
    forwarded = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--binding" and i + 1 < len(args):
            binding = args[i + 1]
            i += 2
            continue
        if arg == "--suite" and i + 1 < len(args):
            suite = args[i + 1]
            i += 2
            continue
        forwarded.append(arg)
        i += 1

    if binding != "cpp":
        print("Error: only --binding cpp is supported.", file=sys.stderr)
        return 1
    if suite not in ("single", "multi"):
        print("Error: --suite must be single or multi.", file=sys.stderr)
        return 1

    script = perf_dir / ("run_binding_multi.sh" if suite == "multi" else "run_binding_single.sh")
    return subprocess.call([str(script), *forwarded])


if __name__ == "__main__":
    raise SystemExit(main())
