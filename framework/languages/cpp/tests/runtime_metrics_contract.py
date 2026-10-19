#!/usr/bin/env python3
"""Check the runtime instrument surface against the shared metric contract."""
import json
import re
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[3]
fixture = json.loads((root / "runtime/protocol/fixtures/runtime-metrics-v1.json").read_text())
expected = {item["name"] for item in fixture["metrics"]}
sources = root / "languages/cpp/framework/src/runtime"
actual = set()
for path in sources.rglob("*"):
    if path.suffix in (".cpp", ".hpp"):
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
        actual.update(re.findall(r'"(zlink\.[a-z_]+(?:\.[a-z_]+)+)"', source))
actual = {name for name in actual if not name.startswith(("zlink.runtime.", "zlink.framework."))}
missing = sorted(expected - actual)
extra = sorted(actual - expected)
if missing or extra:
    print("Missing:", *missing, sep="\n  ")
    print("Outside contract:", *extra, sep="\n  ")
    sys.exit(1)
print(f"Metric names match: {len(expected)}")

