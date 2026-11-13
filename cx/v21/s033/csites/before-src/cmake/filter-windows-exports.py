#!/usr/bin/env python3
"""Restrict CMake's Windows auto-export list to Framework-owned symbols."""

import re
import subprocess
import sys
from pathlib import Path

# An import library holds at most 65535 objects (LNK1189). Fail before that
# with a message that names the cause.
EXPORT_LIMIT = 60000


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
            # Members of class template instances (?name@?$class@...) are defined
            # in headers, so consumers instantiate their own copies.
            if "?$" in symbol.split("@@", 1)[0]:
                continue
            if symbol.startswith("?use_metric_provider@app_t@framework@zlink@@"):
                exports.append(line)
                continue
            if re.search(r"boost|protobuf|absl|opentelemetry", symbol, re.IGNORECASE):
                continue
            if re.search(r"@zlink@@(?:[A-Z0-9]|@8)", symbol):
                exports.append(line)
        path.write_text("\n".join(exports) + "\n", encoding="utf-8")
        print(f"Windows auto-export filter kept {len(exports) - 1} ZLink symbols")
        if len(exports) - 1 > EXPORT_LIMIT:
            print(
                f"error: {len(exports) - 1} exports exceed the {EXPORT_LIMIT} guard "
                "below the 65535 import library limit",
                file=sys.stderr,
            )
            return 1
    return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())
