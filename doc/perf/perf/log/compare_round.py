#!/usr/bin/env python3
"""Compare C baseline vs target binding result file and emit a delta table.

Usage:
  compare_round.py <c_result_file> <target_result_file> [--metric throughput]

Outputs a markdown-ready summary identifying:
- Per cell ratio (target/C * 100)
- Status candidate based on pattern group thresholds for the language group.

Designed for round-2026-05-30 measurement plan.
"""
import argparse
import collections
import re
import sys
from pathlib import Path


PATTERN_GROUPS = {
    "단순 one-way": {"PAIR", "PUBSUB", "DEALER_DEALER", "MULTI_PUBSUB", "MULTI_STREAM", "MULTI_DEALER_DEALER"},
    "routed one-way": {"DEALER_ROUTER", "ROUTER_ROUTER"},
    "multi routed echo": {"MULTI_DEALER_ROUTER", "MULTI_ROUTER_ROUTER"},
    "SPOT 계열": {"SPOT", "MULTI_SPOT", "MULTI_SPOT_REQREP", "MULTI_SPOT_SENDSEND"},
}

# (lang_group, group) -> (min_pct, stable_pct)
THRESHOLDS = {
    ("cpp_rust", "단순 one-way"): (80, 90),
    ("cpp_rust", "routed one-way"): (70, 83),
    ("cpp_rust", "multi routed echo"): (65, 77),
    ("cpp_rust", "SPOT 계열"): (75, 90),
    ("dotnet_java", "단순 one-way"): (63, 73),
    ("dotnet_java", "routed one-way"): (55, 67),
    ("dotnet_java", "multi routed echo"): (50, 63),
    ("dotnet_java", "SPOT 계열"): (60, 70),
    ("go", "단순 one-way"): (53, 63),
    ("go", "routed one-way"): (47, 57),
    ("go", "multi routed echo"): (40, 53),
    ("go", "SPOT 계열"): (50, 60),
    ("node", "단순 one-way"): (30, 43),
    ("node", "routed one-way"): (28, 40),
    ("node", "multi routed echo"): (25, 37),
    ("node", "SPOT 계열"): (28, 40),
    ("node_python", "단순 one-way"): (35, 43),
    ("node_python", "routed one-way"): (33, 40),
    ("node_python", "multi routed echo"): (30, 37),
    ("node_python", "SPOT 계열"): (33, 40),
}


def parse_results(path: Path):
    """Return dict[(pattern, transport, size, metric)] -> value (float)."""
    rows = {}
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if not line.startswith("RESULT,"):
            continue
        parts = line.split(",")
        if len(parts) < 7:
            continue
        # RESULT,<bench>,<pattern>,<transport>,<size>,<metric>,<value>
        _, bench, pattern, transport, size, metric, value = parts[:7]
        try:
            v = float(value)
        except ValueError:
            continue
        rows[(pattern, transport, int(size), metric)] = v
    return rows


def pattern_group(pattern: str):
    for group, members in PATTERN_GROUPS.items():
        if pattern in members:
            return group
    return None


def classify(ratio_pct: float, min_pct: float):
    if ratio_pct >= min_pct:
        return "통과"
    return "미달"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("c_file", type=Path)
    parser.add_argument("target_file", type=Path)
    parser.add_argument("--lang-group", required=True,
                        choices=["cpp_rust", "dotnet_java", "go", "node", "node_python"])
    parser.add_argument("--metric", default="throughput")
    parser.add_argument("--only-fails", action="store_true",
                        help="Print only 미달 rows")
    args = parser.parse_args()

    c_rows = parse_results(args.c_file)
    t_rows = parse_results(args.target_file)

    # Iterate over target rows (matching metric only)
    keys = sorted({k for k in t_rows if k[3] == args.metric})
    summary = collections.Counter()
    fails = []
    out_lines = []
    out_lines.append("| Pattern | Transport | Size | C | Target | Ratio% | Group | Status |")
    out_lines.append("|---|---|---|---|---|---|---|---|")
    for k in keys:
        pattern, transport, size, metric = k
        c_val = c_rows.get(k)
        t_val = t_rows.get(k)
        if c_val is None or t_val is None or c_val == 0:
            continue
        ratio = t_val / c_val * 100.0
        group = pattern_group(pattern)
        if group is None:
            continue
        thr = THRESHOLDS[(args.lang_group, group)]
        status = classify(ratio, thr[0])
        summary[status] += 1
        if status == "미달":
            fails.append((pattern, transport, size, ratio))
        if args.only_fails and status != "미달":
            continue
        out_lines.append(
            f"| {pattern} | {transport} | {size} | {c_val:.1f} | {t_val:.1f} | "
            f"{ratio:.1f}% | {group} ({thr[0]}~{thr[1]}) | {status} |"
        )

    print("\n".join(out_lines))
    print()
    print(f"## Summary: 통과={summary['통과']}, 미달={summary['미달']}")
    if fails:
        print("\n## 미달 cells:")
        for p, t, s, r in fails:
            print(f"- {p} {t} {s}B: {r:.1f}%")


if __name__ == "__main__":
    main()
