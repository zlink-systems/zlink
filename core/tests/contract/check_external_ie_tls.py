#!/usr/bin/env python3
"""Reject static TLS relocations in libzlink that bind to external symbols."""

import pathlib
import re
import subprocess
import sys


RELOCATION = re.compile(r"^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+(R_\S+)")
SYMBOL = re.compile(r"^\s*(\d+):\s+")


def readelf(tool, option, library):
    result = subprocess.run(
        [tool, option, "-W", str(library)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"readelf {option} failed")
    return result.stdout


def is_static_tls_relocation(name):
    # DTPOFF/DTPREL address dynamic TLS and do not consume static TLS surplus.
    return any(
        part.startswith(("TPOFF", "TPREL"))
        for part in name.split("_")
    )


def check(library, tool):
    header = library.read_bytes()[:5]
    if header[:4] != b"\x7fELF" or header[4] not in (1, 2):
        raise RuntimeError(f"not an ELF library: {library}")
    symbol_shift = 32 if header[4] == 2 else 8

    undefined = set()
    symbols = set()
    for line in readelf(tool, "--dyn-syms", library).splitlines():
        match = SYMBOL.match(line)
        if not match:
            continue
        fields = line.split()
        if len(fields) < 7:
            raise RuntimeError(f"cannot parse dynamic symbol: {line}")
        index = int(match.group(1))
        symbols.add(index)
        if fields[6] == "UND":
            undefined.add(index)
    if not symbols:
        raise RuntimeError("readelf returned no dynamic symbols")

    static_tls = []
    external = []
    for line in readelf(tool, "-r", library).splitlines():
        match = RELOCATION.match(line)
        if not match or not is_static_tls_relocation(match.group(2)):
            continue
        static_tls.append(line)
        symbol_index = int(match.group(1), 16) >> symbol_shift
        if symbol_index == 0:
            continue  # STN_UNDEF here denotes a local constant-offset relocation.
        if symbol_index not in symbols:
            raise RuntimeError(f"unknown dynamic symbol in relocation: {line}")
        if symbol_index in undefined:
            external.append(line)

    if external:
        print(f"FAIL: {len(external)} external static TLS relocation(s)")
        for line in external:
            print(line)
        return 1
    print(f"PASS: external static TLS relocations=0; total static TLS relocations={len(static_tls)}")
    return 0


def main():
    if len(sys.argv) != 3:
        print("usage: check_external_ie_tls.py <libzlink.so> <readelf>", file=sys.stderr)
        return 2
    try:
        return check(pathlib.Path(sys.argv[1]), sys.argv[2])
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
