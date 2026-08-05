#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0

"""Verify that bindings with bundled native artifacts carry the same file set."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NATIVE_BINDINGS = ("go", "rust")
MANIFEST = ROOT / "bindings" / "native-artifacts.txt"


def tracked_native_files(binding: str) -> set[str]:
    base = ROOT / "bindings" / binding / "native"
    if not base.exists():
        return set()
    return {
        str(path.relative_to(base))
        for path in base.rglob("*")
        if path.is_file() or path.is_symlink()
    }


def main() -> int:
    by_binding = {binding: tracked_native_files(binding) for binding in NATIVE_BINDINGS}
    reference = {
        line.strip()
        for line in MANIFEST.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    }
    failed = False

    for binding in NATIVE_BINDINGS:
        current = by_binding[binding]
        missing = sorted(reference - current)
        extra = sorted(current - reference)
        if not missing and not extra:
            continue
        failed = True
        print(f"{binding} native artifacts differ from {MANIFEST}", file=sys.stderr)
        for item in missing:
            print(f"  missing: {item}", file=sys.stderr)
        for item in extra:
            print(f"  extra: {item}", file=sys.stderr)

    if failed:
        return 1
    print("native artifact parity OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
