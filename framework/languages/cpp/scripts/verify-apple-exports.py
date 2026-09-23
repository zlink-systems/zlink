#!/usr/bin/env python3
"""Compare Apple's export patterns with the Framework symbols in ELF builds."""

import fnmatch
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <symbol-map> <shared-library>...", file=sys.stderr)
        return 2

    symbol_map = pathlib.Path(sys.argv[1])
    patterns = [
        line.removeprefix("# APPLE_EXPORT: ").strip()
        for line in symbol_map.read_text(encoding="utf-8").splitlines()
        if line.startswith("# APPLE_EXPORT: ")
    ]
    if not patterns:
        print(f"no Apple export patterns in {symbol_map}", file=sys.stderr)
        return 1

    failures = []
    framework_count = 0
    other_count = 0
    for library in sys.argv[2:]:
        output = subprocess.check_output(
            ["nm", "-D", "--defined-only", library], text=True
        )
        symbols = [line.split()[-1] for line in output.splitlines()]
        demangled = subprocess.check_output(
            ["c++filt"], input="\n".join(symbols) + "\n", text=True
        ).splitlines()
        for symbol, name in zip(symbols, demangled, strict=True):
            is_framework = not symbol.startswith("_ZSt") and name.startswith(
                ("zlink::", "vtable for zlink::", "typeinfo for zlink::",
                 "typeinfo name for zlink::")
            )
            # Mach-O adds one underscore to the ELF spelling.
            matches = any(fnmatch.fnmatchcase("_" + symbol, pattern) for pattern in patterns)
            if is_framework:
                framework_count += 1
            else:
                other_count += 1
            if matches != is_framework:
                failures.append(f"{library}: {'missing' if is_framework else 'leaked'} {symbol} ({name})")

    # These names must remain excluded even when a third-party symbol mentions
    # a Framework type in its template arguments or parameters.
    third_party = (
        "_ZN5boost3fooEN5zlink9framework3barE",
        "_ZN6google8protobuf3fooEN5zlink9framework3barE",
        "_ZN4absl3fooEN5zlink9framework3barE",
        "_ZN13opentelemetry3fooEN5zlink9framework3barE",
    )
    for symbol in third_party:
        if any(fnmatch.fnmatchcase("_" + symbol, pattern) for pattern in patterns):
            failures.append(f"third-party pattern leak: {symbol}")

    for failure in failures[:20]:
        print(failure, file=sys.stderr)
    if len(failures) > 20:
        print(f"... {len(failures) - 20} more failures", file=sys.stderr)
    print(f"Apple export check: {framework_count} Framework symbols, "
          f"{other_count} other ELF exports, {len(failures)} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
