#!/usr/bin/env python3
"""Check the runtime instrument surface against the shared metric contract."""
import json
import re
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[3]
fixture = json.loads((root / "runtime/protocol/fixtures/runtime-metrics-v1.json").read_text(encoding="utf-8"))
contract = {item["name"]: item for item in fixture["metrics"]}
expected = set(contract)
sources = root / "languages/cpp/framework/src/runtime"
actual = set()
metadata_checked = set()
metadata_errors = []
instrument_kinds = {
    "Counter": "counter",
    "ObservableCounter": "counter",
    "UpDownCounter": "updown",
    "ObservableGauge": "observable",
    "Histogram": "histogram",
}
for path in sources.rglob("*"):
    if path.suffix in (".cpp", ".hpp"):
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
        actual.update(re.findall(r'"(zlink\.[a-z_]+(?:\.[a-z_]+)+)"', source))
        declarations = [
            (name, instrument_kinds[kind], unit)
            for kind, name, unit in re.findall(
                r'Create(?:Double|Int64|UInt64)(ObservableCounter|UpDownCounter|Counter|'
                r'ObservableGauge|Histogram)\s*\(\s*"(zlink\.[a-z_.]+)"\s*,\s*"[^"]*"\s*,\s*"([^"]*)"',
                source,
            )
        ]
        declarations += [
            (name, kind, unit)
            for kind, name, unit in re.findall(
                r'\.(counter|updown|observable|histogram)\s*\(\s*"(zlink\.[a-z_.]+)"\s*,\s*"([^"]*)"',
                source,
            )
        ]
        declarations += re.findall(
            r'"(zlink\.[a-z_.]+)"\s*,\s*(?:::[\w:]+::)?metric_instrument_kind_t::'
            r'(counter|updown|observable|histogram)\s*,\s*"([^"]*)"',
            source,
        )
        for name, kind, unit in declarations:
            if name not in contract:
                continue
            metadata_checked.add(name)
            if (kind, unit) != (contract[name]["kind"], contract[name]["unit"]):
                metadata_errors.append(f"{path.name}: {name}: {kind}, {unit}")
actual = {name for name in actual if not name.startswith(("zlink.runtime.", "zlink.framework."))}
missing = sorted(expected - actual)
extra = sorted(actual - expected)
if missing or extra or metadata_errors:
    print("Missing:", *missing, sep="\n  ")
    print("Outside contract:", *extra, sep="\n  ")
    print("Incorrect kind/unit:", *metadata_errors, sep="\n  ")
    sys.exit(1)
print(f"Metric names match: {len(expected)}; literal kind/unit declarations: {len(metadata_checked)}")

