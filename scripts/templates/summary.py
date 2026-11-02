#!/usr/bin/env python3
"""Renders the framework-templates.yml results as a Markdown table.

Usage: summary.py <results-dir> <matrix-json>...

Each <matrix-json> is a {"include": [...]} matrix from plan.py; every planned entry gets a
row, and an entry without a result file (job cancelled or failed before the run) shows
"not run".
"""
import json
import sys
from pathlib import Path


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    results = {}
    for path in Path(argv[1]).glob("*.json"):
        entry = json.loads(path.read_text())
        results[(entry["lang"], entry["template"])] = entry
    planned = [e for matrix in argv[2:] for e in json.loads(matrix)["include"]]
    lines = ["## Framework templates", "",
             "| Language | Template | Build | E2E | Scenarios |", "|---|---|---|---|---|"]
    passed = 0
    for e in sorted(planned, key=lambda e: (e["lang"], e["template"])):
        r = results.get((e["lang"], e["template"]))
        if r is None:
            lines.append(f"| {e['lang']} | {e['template']} | not run | not run | |")
            continue
        passed += r["build"] == "pass" and r["e2e"] == "pass"
        lines.append(f"| {r['lang']} | {r['template']} | {r['build']} | {r['e2e']} | {r['scenarios']} |")
    lines += ["", f"{passed}/{len(planned)} templates passed build and e2e."]
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv)
