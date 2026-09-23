#!/usr/bin/env python3
"""Restrict CMake's Windows auto-export list to Framework-owned symbols."""

import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    command = sys.argv[1:]
    definition = next(
        (argument[5:] for argument in command if argument.upper().startswith("/DEF:")),
        None,
    )
    if definition:
        path = Path(definition)
        lines = path.read_text(encoding="utf-8").splitlines()
        exports = ["EXPORTS"]
        for line in lines:
            symbol = line.strip().split(maxsplit=1)[0] if line.strip() else ""
            if symbol.startswith("??$"):
                continue
            if re.search(r"boost|protobuf|absl|opentelemetry", symbol, re.IGNORECASE):
                continue
            if re.search(r"@zlink@@(?:[A-Z0-9]|@8)", symbol):
                exports.append(line)
        path.write_text("\n".join(exports) + "\n", encoding="utf-8")
        print(f"Windows auto-export filter kept {len(exports) - 1} ZLink symbols")
    return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())
