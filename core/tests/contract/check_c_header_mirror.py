#!/usr/bin/env python3

import pathlib
import sys


def relative_files(root: pathlib.Path) -> set[pathlib.Path]:
    return {path.relative_to(root) for path in root.rglob("*") if path.is_file()}


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    core_headers = root / "core/include"
    binding_headers = root / "bindings/c/include"

    core_files = relative_files(core_headers)
    binding_files = relative_files(binding_headers)
    missing = sorted(core_files - binding_files)
    extra = sorted(binding_files - core_files)
    changed = sorted(
        path
        for path in core_files & binding_files
        if (core_headers / path).read_bytes() != (binding_headers / path).read_bytes()
    )

    for path in missing:
        print(f"missing C binding header: {path}", file=sys.stderr)
    for path in extra:
        print(f"extra C binding header: {path}", file=sys.stderr)
    for path in changed:
        print(f"C binding header differs from Core: {path}", file=sys.stderr)
    return 1 if missing or extra or changed else 0


if __name__ == "__main__":
    raise SystemExit(main())
