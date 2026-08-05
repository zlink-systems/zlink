#!/usr/bin/env python3
import argparse
import csv
import json
import os
import statistics
from collections import defaultdict


def parse_list(text, cast):
    out = []
    for token in str(text).split(","):
        token = token.strip()
        if not token:
            continue
        out.append(cast(token))
    return out


def fnum(row, key):
    try:
        return float(row.get(key, ""))
    except Exception:
        return None


def inum(row, key):
    try:
        return int(str(row.get(key, "")).strip())
    except Exception:
        return 0


def tps_or_none(row, size):
    direct = fnum(row, "throughput_tps")
    if direct is not None:
        return direct
    bps = fnum(row, "throughput_bps")
    if bps is None or size <= 0:
        return None
    return bps / float(size)


def kops_or_none(tps_value):
    if tps_value is None:
        return None
    return tps_value / 1000.0


def median_or_none(values):
    if not values:
        return None
    return statistics.median(values)


def load_skips(path):
    skips = []
    if not path or not os.path.exists(path):
        return skips
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            stack = row.get("stack", "").strip()
            reason = row.get("reason", "").strip()
            if stack:
                skips.append({"stack": stack, "reason": reason})
    return skips


def ranking_sort_key(phase, entry):
    median_tps = entry.get("median_tps")
    median_tps_value = median_tps if median_tps is not None else 0.0
    median = entry.get("median_bps")
    median_bps = median if median is not None else 0.0
    peak_tps = entry.get("peak_tps")
    peak_tps_value = peak_tps if peak_tps is not None else 0.0
    peak = entry.get("peak_bps") or 0.0
    mean = entry.get("median_mean_us")
    p95 = entry.get("median_p95_us")
    if phase == "latency":
        return (
            mean if mean is not None else 1e30,
            -median_tps_value,
            -median_bps,
            -peak_tps_value,
            -peak,
        )
    return (
        -median_tps_value,
        -median_bps,
        -peak_tps_value,
        -peak,
        p95 if p95 is not None else 1e30,
    )


def main():
    parser = argparse.ArgumentParser(description="Summarize stream compare metrics")
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--stacks", default="")
    parser.add_argument("--sizes", default="")
    parser.add_argument("--phases", default="throughput,latency")
    parser.add_argument("--skip-file", default="")
    args = parser.parse_args()

    selected_stacks = parse_list(args.stacks, str)
    selected_sizes = parse_list(args.sizes, int)
    selected_phases = parse_list(args.phases, str)
    if not selected_phases:
        selected_phases = ["throughput", "latency"]

    skips = load_skips(args.skip_file)

    rows = []
    with open(args.metrics, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        required = ["phase", "size_mismatch"]
        for col in required:
            if col not in fieldnames:
                raise SystemExit(f"metrics csv missing required column: {col}")

        for row in reader:
            phase = row.get("phase", "").strip()
            if selected_phases and phase not in selected_phases:
                continue
            rows.append(row)

    grouped = defaultdict(list)
    for row in rows:
        stack = row.get("stack", "")
        phase = row.get("phase", "")
        try:
            size = int(row.get("size", "0"))
        except Exception:
            continue
        if selected_stacks and stack not in selected_stacks:
            continue
        if selected_sizes and size not in selected_sizes:
            continue
        grouped[(phase, stack, size)].append(row)

    summary = {
        "config": {
            "runs": args.runs,
            "stacks": selected_stacks,
            "sizes": selected_sizes,
            "phases": selected_phases,
        },
        "skipped_stacks": skips,
        "warnings": [],
        "metrics": {},
        "ranking": {},
    }

    for phase in selected_phases:
        for stack in selected_stacks:
            for size in selected_sizes:
                key = (phase, stack, size)
                items = grouped.get(key, [])
                total = len(items)
                pass_rows = [
                    r
                    for r in items
                    if r.get("pass_fail") == "PASS" and r.get("client_rc") == "0"
                ]

                throughputs = []
                tps_values = []
                mean_values = []
                p95s = []
                server_avg_cpu = []
                server_peak_rss_kb = []
                client_avg_cpu = []
                client_peak_rss_kb = []
                system_avg_cpu = []
                system_peak_mem_used_pct = []
                mismatch_total = 0

                for row in pass_rows:
                    tp = fnum(row, "throughput_bps")
                    mean = fnum(row, "p50_us")
                    p95 = fnum(row, "p95_us")
                    if tp is None:
                        continue
                    throughputs.append(tp)
                    if mean is not None:
                        mean_values.append(mean)
                    if p95 is not None:
                        p95s.append(p95)
                    tps = tps_or_none(row, size)
                    if tps is not None:
                        tps_values.append(tps)

                    mismatch_total += inum(row, "size_mismatch")

                    v = fnum(row, "server_avg_cpu_pct")
                    if v is not None:
                        server_avg_cpu.append(v)
                    v = fnum(row, "server_peak_rss_kb")
                    if v is not None:
                        server_peak_rss_kb.append(v)
                    v = fnum(row, "client_avg_cpu_pct")
                    if v is not None:
                        client_avg_cpu.append(v)
                    v = fnum(row, "client_peak_rss_kb")
                    if v is not None:
                        client_peak_rss_kb.append(v)
                    v = fnum(row, "system_avg_cpu_pct")
                    if v is not None:
                        system_avg_cpu.append(v)
                    v = fnum(row, "system_peak_mem_used_pct")
                    if v is not None:
                        system_peak_mem_used_pct.append(v)

                mismatch_rows = sum(1 for row in items if inum(row, "size_mismatch") > 0)
                mismatch_total_all = sum(inum(row, "size_mismatch") for row in items)

                peak = max(throughputs) if throughputs else None
                median = statistics.median(throughputs) if throughputs else None
                peak_tps = max(tps_values) if tps_values else None
                median_tps = statistics.median(tps_values) if tps_values else None
                median_mean = statistics.median(mean_values) if mean_values else None
                median_p95 = statistics.median(p95s) if p95s else None

                pass_rate = (len(pass_rows) / total) if total > 0 else 0.0

                metric_key = f"{phase}:{stack}:{size}"
                summary["metrics"][metric_key] = {
                    "phase": phase,
                    "total_runs": total,
                    "pass_runs": len(pass_rows),
                    "pass_rate": pass_rate,
                    "peak_bps": peak,
                    "median_bps": median,
                    "peak_tps": peak_tps,
                    "median_tps": median_tps,
                    "peak_kops": kops_or_none(peak_tps),
                    "median_kops": kops_or_none(median_tps),
                    "median_mean_us": median_mean,
                    "median_p95_us": median_p95,
                    "mismatch_total": mismatch_total,
                    "mismatch_total_all": mismatch_total_all,
                    "mismatch_rows": mismatch_rows,
                    "median_server_avg_cpu_pct": median_or_none(server_avg_cpu),
                    "median_server_peak_rss_kb": median_or_none(server_peak_rss_kb),
                    "median_client_avg_cpu_pct": median_or_none(client_avg_cpu),
                    "median_client_peak_rss_kb": median_or_none(client_peak_rss_kb),
                    "median_system_avg_cpu_pct": median_or_none(system_avg_cpu),
                    "median_system_peak_mem_used_pct": median_or_none(system_peak_mem_used_pct),
                }

                if mismatch_total_all > 0:
                    summary["warnings"].append(
                        {
                            "phase": phase,
                            "stack": stack,
                            "size": size,
                            "size_mismatch": mismatch_total_all,
                            "message": "size mismatch observed (drop-and-continue)",
                        }
                    )

    for phase in selected_phases:
        for size in selected_sizes:
            ranking = []
            for stack in selected_stacks:
                item = summary["metrics"].get(f"{phase}:{stack}:{size}")
                if not item:
                    continue
                if phase == "throughput" and item.get("median_bps") is None:
                    continue
                if phase == "latency" and item.get("median_mean_us") is None:
                    continue
                entry = {
                    "stack": stack,
                    "median_bps": item.get("median_bps"),
                    "median_tps": item.get("median_tps"),
                    "median_kops": item.get("median_kops"),
                    "peak_bps": item.get("peak_bps"),
                    "peak_tps": item.get("peak_tps"),
                    "peak_kops": item.get("peak_kops"),
                    "median_mean_us": item.get("median_mean_us"),
                    "median_p95_us": item.get("median_p95_us"),
                    "mismatch_total_all": item.get("mismatch_total_all", 0),
                }
                ranking.append(entry)

            ranking.sort(key=lambda x: ranking_sort_key(phase, x))

            summary["ranking"][f"{phase}:{size}"] = ranking

    os.makedirs(os.path.dirname(args.summary), exist_ok=True)
    with open(args.summary, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    lines = []
    lines.append("# Stream Compare Report")
    lines.append("")
    lines.append(f"- runs: {args.runs}")
    lines.append(f"- phases: {','.join(selected_phases)}")
    if args.runs <= 1:
        lines.append("- warning: single-run mode is noise-sensitive")
    lines.append("")

    if skips:
        lines.append("## Skipped Stacks")
        lines.append("")
        for skip in skips:
            lines.append(f"- {skip['stack']}: {skip['reason']}")
        lines.append("")

    if summary["warnings"]:
        lines.append("## Mismatch Warnings")
        lines.append("")
        for warning in summary["warnings"]:
            lines.append(
                f"- phase={warning['phase']} stack={warning['stack']} size={warning['size']} size_mismatch={warning['size_mismatch']}"
            )
        lines.append("")

    for phase in selected_phases:
        lines.append(f"## Metrics (phase={phase})")
        lines.append("")
        lines.append("| stack | size | pass/total | peak bps | peak kops | median bps | median kops | mean(ms) | mismatch | srv cpu% | srv rss(MiB) | cli cpu% | cli rss(MiB) | sys cpu% | sys mem%(peak) |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for size in selected_sizes:
            for stack in selected_stacks:
                item = summary["metrics"].get(f"{phase}:{stack}:{size}")
                if not item:
                    lines.append(f"| {stack} | {size} | 0/0 | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |")
                    continue
                srv_cpu = item.get("median_server_avg_cpu_pct")
                srv_rss_kb = item.get("median_server_peak_rss_kb")
                cli_cpu = item.get("median_client_avg_cpu_pct")
                cli_rss_kb = item.get("median_client_peak_rss_kb")
                sys_cpu = item.get("median_system_avg_cpu_pct")
                sys_mem_peak = item.get("median_system_peak_mem_used_pct")
                lines.append(
                    "| {stack} | {size} | {pass_runs}/{total_runs} | {peak} | {peak_kops} | {median} | {median_kops} | {mean_ms} | {mismatch} | {srv_cpu} | {srv_rss} | {cli_cpu} | {cli_rss} | {sys_cpu} | {sys_mem_peak} |".format(
                        stack=stack,
                        size=size,
                        pass_runs=item.get("pass_runs", 0),
                        total_runs=item.get("total_runs", 0),
                        peak=(f"{item['peak_bps']:.2f}" if item.get("peak_bps") is not None else "n/a"),
                        peak_kops=(f"{item['peak_kops']:.2f}" if item.get("peak_kops") is not None else "n/a"),
                        median=(f"{item['median_bps']:.2f}" if item.get("median_bps") is not None else "n/a"),
                        median_kops=(f"{item['median_kops']:.2f}" if item.get("median_kops") is not None else "n/a"),
                        mean_ms=(
                            f"{(item['median_mean_us'] / 1000.0):.2f}"
                            if item.get("median_mean_us") is not None
                            else "n/a"
                        ),
                        mismatch=item.get("mismatch_total_all", 0),
                        srv_cpu=(f"{srv_cpu:.2f}" if srv_cpu is not None else "n/a"),
                        srv_rss=(f"{(srv_rss_kb / 1024.0):.2f}" if srv_rss_kb is not None else "n/a"),
                        cli_cpu=(f"{cli_cpu:.2f}" if cli_cpu is not None else "n/a"),
                        cli_rss=(f"{(cli_rss_kb / 1024.0):.2f}" if cli_rss_kb is not None else "n/a"),
                        sys_cpu=(f"{sys_cpu:.2f}" if sys_cpu is not None else "n/a"),
                        sys_mem_peak=(f"{sys_mem_peak:.2f}" if sys_mem_peak is not None else "n/a"),
                    )
                )

        for size in selected_sizes:
            lines.append("")
            lines.append(f"## Ranking (phase={phase}, size={size})")
            lines.append("")
            lines.append("| rank | stack | median bps | median kops | mean(ms) | mismatch |")
            lines.append("|---:|---|---:|---:|---:|---:|")
            ranking = summary["ranking"].get(f"{phase}:{size}", [])
            if not ranking:
                lines.append("| 1 | n/a | n/a | n/a | n/a | n/a |")
            else:
                for idx, entry in enumerate(ranking, 1):
                    median_kops = entry.get("median_kops")
                    median_kops_text = (
                        f"{median_kops:.2f}" if median_kops is not None else "n/a"
                    )
                    mean = entry.get("median_mean_us")
                    mean_text = f"{(mean / 1000.0):.2f}" if mean is not None else "n/a"
                    mismatch = entry.get("mismatch_total_all", 0)
                    median_bps = entry.get("median_bps")
                    median_bps_text = (
                        f"{median_bps:.2f}" if median_bps is not None else "n/a"
                    )
                    lines.append(
                        f"| {idx} | {entry['stack']} | {median_bps_text} | "
                        f"{median_kops_text} | {mean_text} | {mismatch} |"
                    )
        lines.append("")

    with open(args.report, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
