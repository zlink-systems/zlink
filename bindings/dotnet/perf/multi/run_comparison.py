#!/usr/bin/env python3
import os
import subprocess
import sys


def main() -> int:
    script = os.path.join(os.path.dirname(__file__), "..", "run_comparison.py")
    return subprocess.call([sys.executable, script] + sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
