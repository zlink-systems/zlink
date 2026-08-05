import argparse
import csv
import math
import os
import platform
import subprocess
import sys
from collections import defaultdict
from datetime import datetime


METRIC_ORDER = {
    "bandwidth": 0,
    "latency": 1,
    "latency_p95": 2,
    "latency_p99": 3,
    "throughput": 4,
}

REQUIRED_METRICS = ("throughput", "bandwidth", "latency", "latency_p95", "latency_p99")
RESULT_METRICS = ("bandwidth", "latency", "latency_p95", "latency_p99", "throughput")
ECHO_MULTI_PATTERNS = {
    "MULTI_DEALER_ROUTER",
    "MULTI_ROUTER_ROUTER",
    "MULTI_STREAM",
}

SINGLE_TABLE_HEADER_LINES = (
    "| Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |",
    "|----------|------------------|--------------|--------------|--------------|--------------|",
)

MULTI_TABLE_HEADER_LINES = (
    "| Size     |         Throughput |      Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |",
    "|----------|--------------------|----------------|---------------|---------------|---------------|",
)


def parse_csv(value):
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def parse_auto_hwm_detail_line(line):
    stripped = (line or "").strip()
    if not stripped.startswith("AUTO_HWM_DETAIL,"):
        return None
    fields = {}
    for item in stripped.split(",")[1:]:
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields or None


def auto_hwm_bytes_to_kb_display(value):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return "?"
    if parsed <= 0:
        return "0"
    if parsed % 1024 == 0:
        return str(parsed // 1024)
    return f"{parsed / 1024.0:.1f}"


def load_auto_hwm_detail_rows(path):
    if not path:
        return []
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parsed = parse_auto_hwm_detail_line(line)
            if parsed:
                rows.append(parsed)
    return rows


def single_auto_hwm_detail_lines(patterns, msg_sizes, rows=None):
    rows = rows or []
    yield "## Auto-HWM Detail"
    for pattern in patterns:
        yield f"- pattern: {pattern}"
        yield (
            "| Size(B) | Component | Owner | Socket | Type | Role | SNDHWM | RCVHWM | "
            "SNDBUF(KB) | RCVBUF(KB) | MsgUnit(B) | Slots |"
        )
        yield (
            "|---------|-----------|-------|--------|------|------|--------|--------|"
            "-----------|-----------|------------|-------|"
        )
        display_rows = []
        seen = set()
        for row in rows:
            if row.get("pattern", "").upper() != pattern.upper():
                continue
            display = dict(row)
            display["sndbuf_kb"] = auto_hwm_bytes_to_kb_display(row.get("effective_sndbuf", ""))
            display["rcvbuf_kb"] = auto_hwm_bytes_to_kb_display(row.get("effective_rcvbuf", ""))
            key = (
                display.get("msg_size", ""),
                display.get("component", ""),
                display.get("owner", ""),
                display.get("socket", ""),
                display.get("socket_type", ""),
                display.get("role", ""),
                display.get("sndhwm", ""),
                display.get("rcvhwm", ""),
                display.get("sndbuf_kb", ""),
                display.get("rcvbuf_kb", ""),
                display.get("effective_message_bytes", ""),
                display.get("socket_message_slots", ""),
            )
            if key in seen:
                continue
            seen.add(key)
            display_rows.append(display)
        display_rows.sort(
            key=lambda row: (
                int(row.get("msg_size", "0") or "0"),
                row.get("component", ""),
                row.get("owner", ""),
                row.get("socket", ""),
            )
        )
        if display_rows:
            for row in display_rows:
                yield (
                    f"| {row.get('msg_size', '?')} | {row.get('component', '?')} | "
                    f"{row.get('owner', '?')} | {row.get('socket', '?')} | "
                    f"{row.get('socket_type', '?')} | {row.get('role', '?')} | "
                    f"{row.get('sndhwm', '?')} | {row.get('rcvhwm', '?')} | "
                    f"{row.get('sndbuf_kb', '?')} | {row.get('rcvbuf_kb', '?')} | "
                    f"{row.get('effective_message_bytes', '?')} | "
                    f"{row.get('socket_message_slots', '?')} |"
                )
            continue
        for msg_size in msg_sizes:
            yield (
                f"| {msg_size} | unavailable | binding | n/a | n/a | n/a | "
                "n/a | n/a | n/a | n/a | n/a | n/a |"
            )


def multi_auto_hwm_lines(pattern, msg_sizes):
    yield "    Auto-HWM detail:"
    yield (
        "      | Size(B) | Component   | Type | UnitBudget(KB) | MsgUnit(B) | "
        "SNDHWM | RCVHWM | SNDBUF(KB) | RCVBUF(KB) |"
    )
    yield (
        "      |---------|-------------|------|----------------|------------|--------|"
        "--------|------------|------------|"
    )
    for msg_size in msg_sizes:
        yield (
            f"      | {msg_size:<7} | unavailable | n/a  | n/a            | "
            f"{msg_size:<10} | n/a    | n/a    | n/a        | n/a        |"
        )


def sort_result_data_lines(lines):
    def sort_key(line):
        parts = line.strip().split(",")
        if len(parts) >= 7 and parts[0] == "RESULT":
            try:
                size = int(parts[4])
            except ValueError:
                size = -1
            return (parts[2], parts[3], size, METRIC_ORDER.get(parts[5], 99), line)
        return (
            parts[2] if len(parts) > 2 else "",
            parts[3] if len(parts) > 3 else "",
            -1,
            99,
            line,
        )

    selected = [
        line.rstrip("\n")
        for line in lines
        if line.startswith("RESULT,")
    ]
    return sorted(selected, key=sort_key)


def _read_metric_case_files(metrics_path, cases_path):
    rows = defaultdict(lambda: defaultdict(list))
    cases = {}
    patterns = []
    pattern_transports = defaultdict(list)
    pattern_sizes = defaultdict(list)

    with open(metrics_path, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        for pattern, transport, size, _run, metric, value in reader:
            size = int(size)
            key = (pattern, transport, size)
            if pattern not in patterns:
                patterns.append(pattern)
            if transport not in pattern_transports[pattern]:
                pattern_transports[pattern].append(transport)
            if size not in pattern_sizes[pattern]:
                pattern_sizes[pattern].append(size)
            try:
                rows[key][metric].append(float(value))
            except ValueError:
                rows[key][metric].append(math.nan)

    with open(cases_path, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        for pattern, transport, size, status, reason in reader:
            size = int(size)
            cases[(pattern, transport, size)] = (status, reason)
            if pattern not in patterns:
                patterns.append(pattern)
            if transport not in pattern_transports[pattern]:
                pattern_transports[pattern].append(transport)
            if size not in pattern_sizes[pattern]:
                pattern_sizes[pattern].append(size)

    for pattern in pattern_sizes:
        pattern_sizes[pattern].sort()
    return rows, cases, patterns, pattern_transports, pattern_sizes


def _median(values):
    usable = [value for value in values if not math.isnan(value)]
    if not usable:
        return math.nan
    usable.sort()
    mid = len(usable) // 2
    if len(usable) % 2:
        return usable[mid]
    return (usable[mid - 1] + usable[mid]) / 2.0


def _fmt_rate(value, precision):
    return "N/A" if math.isnan(value) else f"{value / 1000.0:.{precision}f}"


def _fmt_bandwidth(value, precision):
    return "N/A" if math.isnan(value) else f"{value:.{precision}f} MB/s"


def _fmt_latency_ms(value):
    return "N/A" if math.isnan(value) else f"{value:.3f} ms"


def _fmt_metric(value):
    return "N/A" if math.isnan(value) else f"{value:.3f}"


def _write_report(lines, report_path, output_path):
    text = "\n".join(lines) + "\n"
    with open(report_path, "w", encoding="utf-8") as fh:
        fh.write(text)
    if output_path:
        with open(output_path, "w", encoding="utf-8") as fh:
            fh.write(text)
    sys.stdout.write(text)


def _single_direction(_pattern):
    return "one-way"


def _multi_direction(pattern):
    return "echo" if pattern in ECHO_MULTI_PATTERNS else "one-way"


def _rate_unit(pattern, direction_func):
    return "Kops/s" if direction_func(pattern) == "echo" else "Kmsg/s"


def _metric_value_for_run(rows, key, metric, run_index):
    values = rows[key].get(metric, [])
    if run_index < len(values):
        return values[run_index]
    return math.nan


def _single_case_row(pattern, size, metric_values):
    throughput = f"{_fmt_rate(metric_values['throughput'], 2):>8} {_rate_unit(pattern, _single_direction)}"
    return (
        f"      | {str(size) + 'B':<8} | {throughput:>16} | "
        f"{_fmt_bandwidth(metric_values['bandwidth'], 2):>12} | "
        f"{_fmt_latency_ms(metric_values['latency']):>12} | "
        f"{_fmt_latency_ms(metric_values['latency_p95']):>12} | "
        f"{_fmt_latency_ms(metric_values['latency_p99']):>12} |"
    )


def _multi_case_row(pattern, size, metric_values):
    throughput = f"{_fmt_rate(metric_values['throughput'], 3):>8} {_rate_unit(pattern, _multi_direction)}"
    return (
        f"      | {str(size) + 'B':<8} | {throughput:>16} | "
        f"{metric_values['bandwidth']:10.3f} MB/s | "
        f"{_fmt_latency_ms(metric_values['latency']):>12} | "
        f"{_fmt_latency_ms(metric_values['latency_p95']):>12} | "
        f"{_fmt_latency_ms(metric_values['latency_p99']):>12} |"
    )


def _single_effective_options(args, section):
    lines = [
        f"## Effective Options ({section})",
        f"- lang: {args.lang}",
        "- suite: single",
        f"- runs: {args.runs}",
        f"- duration_seconds: {args.duration}",
        "- timeout_seconds: 30",
        f"- fail_fast: {args.fail_fast}",
        f"- io_threads: {args.io_threads or '1'}",
        f"- hwm: {args.hwm or 'auto-hwm'}",
        f"- sndhwm: {args.send_hwm or args.hwm or 'auto-hwm'}",
        f"- rcvhwm: {args.recv_hwm or args.hwm or 'auto-hwm'}",
        "- sndbuf: -1",
        "- rcvbuf: -1",
        f"- sndtimeo_ms: {args.sndtimeo_ms}",
        f"- rcvtimeo_ms: {args.rcvtimeo_ms}",
        "- ctx_auto_hwm_enable: core-default",
        "- ctx_auto_hwm_profile: balanced",
        f"- patterns: {args.patterns}",
        f"- transports: {args.transports}",
        f"- msg_sizes: {args.msg_sizes}",
    ]
    if section == "result":
        lines.append("")
    return lines


def render_single_report(args):
    rows, cases, patterns, pattern_transports, pattern_sizes = _read_metric_case_files(
        args.metrics, args.cases
    )
    runs = int(args.runs)
    expected = 0
    actual = 0
    lines = []
    result_lines = []
    failures = []

    lines.extend(_single_effective_options(args, "start"))
    for pattern_index, pattern in enumerate(patterns):
        if pattern_index:
            lines.extend(["===============================================================================", ""])
        lines.append(f"## PATTERN: {pattern} ({_single_direction(pattern)})")
        lines.append(f"  > Benchmarking current for {pattern}...")
        for transport in pattern_transports[pattern]:
            lines.append(f"    Testing {transport}:")
            if runs > 1:
                for run_index in range(runs):
                    lines.append(f"      run {run_index + 1}/{runs}:")
                    for header in SINGLE_TABLE_HEADER_LINES:
                        lines.append(f"        {header}")
                    for size in pattern_sizes[pattern]:
                        key = (pattern, transport, size)
                        status, _reason = cases.get(key, ("fail", "missing_case_status"))
                        if status == "unsupported":
                            lines.append(
                                f"        | {size}B | UNSUPPORTED | UNSUPPORTED | "
                                "UNSUPPORTED | UNSUPPORTED | UNSUPPORTED |"
                            )
                            continue
                        metric_values = {
                            metric: _metric_value_for_run(rows, key, metric, run_index)
                            for metric in REQUIRED_METRICS
                        }
                        if any(math.isnan(value) for value in metric_values.values()):
                            lines.append(f"        | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                        else:
                            lines.append(_single_case_row(pattern, size, metric_values))
                lines.append("      median:")
            for header in SINGLE_TABLE_HEADER_LINES:
                lines.append(f"      {header}")
            for size in pattern_sizes[pattern]:
                key = (pattern, transport, size)
                status, reason = cases.get(key, ("fail", "missing_case_status"))
                if status == "unsupported":
                    lines.append(
                        f"      | {size}B | UNSUPPORTED | UNSUPPORTED | "
                        "UNSUPPORTED | UNSUPPORTED | UNSUPPORTED |"
                    )
                    continue
                expected += len(REQUIRED_METRICS)
                metric_values = {
                    metric: _median(rows[key].get(metric, [])) for metric in REQUIRED_METRICS
                }
                complete_case = all(
                    len(rows[key].get(metric, [])) == runs for metric in REQUIRED_METRICS
                )
                if complete_case:
                    actual += len(REQUIRED_METRICS)
                    lines.append(_single_case_row(pattern, size, metric_values))
                    for metric in REQUIRED_METRICS:
                        result_lines.append(
                            f"RESULT,current,{pattern},{transport},{size},{metric},{_fmt_metric(metric_values[metric])}"
                        )
                else:
                    lines.append(f"      | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                    failures.append(
                        f"{pattern} current {transport} {size}B: {reason or 'missing_result_lines'}"
                    )
            lines.append(f"    Testing {transport}: Done")
        lines.append("")

    if failures:
        lines.extend(["## Failures"])
        lines.extend(f"- {failure}" for failure in failures)
        lines.append("")
    lines.extend(single_auto_hwm_detail_lines(patterns, parse_csv(args.msg_sizes)))
    lines.append("")
    lines.extend(_single_effective_options(args, "result"))
    if result_lines:
        lines.append("## Result Data")
        lines.extend(result_lines)
    lines.extend(["", "## Completion"])
    lines.append(f"- status: {'complete' if expected == actual else 'partial'}")
    lines.append(f"- expected_result_lines: {expected}")
    lines.append(f"- actual_result_lines: {actual}")
    status = "complete" if expected == actual else "partial"
    lines.extend(["", f"Saved result file: {args.report} (status={status})"])
    _write_report(lines, args.report, args.output)
    return 0 if expected == actual else 1


def _cpu_name():
    try:
        with open("/proc/cpuinfo", encoding="utf-8", errors="ignore") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _git_commit():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return "unknown"


def _multi_effective_options(args, section):
    default_clients = os.environ.get("PERF_MULTI_DEFAULT_CLIENTS") or os.environ.get(
        "PERF_DEFAULT_CLIENTS", "100"
    )
    default_stream_clients = os.environ.get("PERF_MULTI_DEFAULT_STREAM_CLIENTS") or os.environ.get(
        "PERF_STREAM_DEFAULT_CLIENTS", "10000"
    )
    default_io_threads_value = os.environ.get("PERF_MULTI_DEFAULT_IO_THREADS") or os.environ.get(
        "PERF_DEFAULT_IO_THREADS"
    )
    if default_io_threads_value:
        default_io_threads = f"{default_io_threads_value} (default)"
    else:
        default_io_threads = "4 (python default)" if args.lang == "python" else "4 (default)"
    lines = [
        f"## Effective Options ({section})",
        f"- lang: {args.lang}",
        "- suite: multi",
        f"- runs: {args.runs}",
        f"- patterns: {args.patterns}",
        f"- transports: {args.transports}",
        f"- msg_sizes: {args.msg_sizes}",
        f"- duration_seconds: {args.duration}",
        f"- fail_fast: {args.fail_fast}",
        f"- clients: {args.clients}",
        f"- default_clients: {default_clients}",
        f"- default_stream_clients: {default_stream_clients}",
        f"- server_io_threads: {args.server_io_threads or args.common_io_threads or default_io_threads}",
        f"- client_io_threads: {args.client_io_threads or args.common_io_threads or default_io_threads}",
        f"- hwm: {args.hwm or 'auto-hwm'}",
        f"- sndhwm: {args.send_hwm or args.hwm or 'auto-hwm'}",
        f"- rcvhwm: {args.recv_hwm or args.hwm or 'auto-hwm'}",
        "- sndbuf: -1",
        "- rcvbuf: -1",
        "- ctx_auto_hwm_enable: 1",
        "- ctx_auto_hwm_profile: balanced",
        f"- sndtimeo_ms: {args.sndtimeo_ms}",
        f"- rcvtimeo_ms: {args.rcvtimeo_ms}",
        f"- connect_concurrency: {args.connect_concurrency}",
        "- connect_ready_timeout_ms: 1000",
        "- monitor_hwm: 1000",
        "- server_ready_timeout_ms: 10000",
        "- server_shutdown_timeout_ms: 5000",
        "- server_bind_port: 0",
        f"- transport_transition_ms: {args.transport_transition_ms}",
        f"- pattern_transition_ms: {args.pattern_transition_ms}",
        "- lat_timeout_ms: 5000",
        "- stream_non_tcp_clients_max: 10000",
        "- disable_resource_metrics: 0",
        "- timeout_seconds: auto",
    ]
    if section == "result":
        lines.append("")
    return lines


def render_multi_report(args):
    rows, cases, patterns, pattern_transports, pattern_sizes = _read_metric_case_files(
        args.metrics, args.cases
    )
    runs = int(args.runs)
    expected = 0
    actual = 0
    lines = []
    result_lines = []
    failures = []
    skips = []

    try:
        load_avg = " ".join(f"{value:.2f}" for value in os.getloadavg())
    except OSError:
        load_avg = "unknown"
    lines.extend(
        [
            f"META,os,{platform.system()} {platform.release()}",
            f"META,cpu,{_cpu_name()}",
            f"META,cores,{os.cpu_count() or 0}",
            "META,build,Release",
            f"META,commit,{_git_commit()}",
            f"META,timestamp,{datetime.now().astimezone().isoformat(timespec='seconds')}",
            f"META,load_avg,{load_avg}",
            f"META,runs,{runs}",
            f"META,clients,{args.clients}",
            "",
        ]
    )
    lines.extend(_multi_effective_options(args, "start"))

    for pattern_index, pattern in enumerate(patterns):
        if pattern_index:
            lines.extend(["===============================================================================", ""])
        lines.append(f"## PATTERN: {pattern} ({_multi_direction(pattern)})")
        lines.append(f"  > Benchmarking current for {pattern}...")
        for transport in pattern_transports[pattern]:
            lines.append(f"    Testing {transport}:")
            if runs > 1:
                for run_index in range(runs):
                    lines.append(f"      run {run_index + 1}/{runs}:")
                    for header in MULTI_TABLE_HEADER_LINES:
                        lines.append(f"        {header}")
                    for size in pattern_sizes[pattern]:
                        key = (pattern, transport, size)
                        status, _reason = cases.get(key, ("fail", "missing_case_status"))
                        if status == "unsupported":
                            lines.append(
                                f"        | {size}B | UNSUPPORTED | UNSUPPORTED | "
                                "UNSUPPORTED | UNSUPPORTED | UNSUPPORTED |"
                            )
                            continue
                        if status == "skip":
                            lines.append(f"        | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                            continue
                        metric_values = {
                            metric: _metric_value_for_run(rows, key, metric, run_index)
                            for metric in REQUIRED_METRICS
                        }
                        if any(math.isnan(value) for value in metric_values.values()):
                            lines.append(f"        | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                        else:
                            lines.append(_multi_case_row(pattern, size, metric_values))
                lines.append("      median:")
            for header in MULTI_TABLE_HEADER_LINES:
                lines.append(f"      {header}")
            for size in pattern_sizes[pattern]:
                key = (pattern, transport, size)
                lines.append(f"    Testing {transport} | {size}B:")
                status, reason = cases.get(key, ("fail", "missing_case_status"))
                if status == "unsupported":
                    lines.append(
                        f"      | {size}B | UNSUPPORTED | UNSUPPORTED | "
                        "UNSUPPORTED | UNSUPPORTED | UNSUPPORTED |"
                    )
                    continue
                if status == "skip":
                    lines.append(f"      | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                    skips.append(f"{pattern} {transport} {size}B: {reason or 'skip'}")
                    continue
                expected += len(REQUIRED_METRICS)
                metric_values = {
                    metric: _median(rows[key].get(metric, [])) for metric in REQUIRED_METRICS
                }
                complete_case = all(
                    len(rows[key].get(metric, [])) == runs for metric in REQUIRED_METRICS
                )
                if complete_case:
                    actual += len(REQUIRED_METRICS)
                    lines.append(_multi_case_row(pattern, size, metric_values))
                    for metric in RESULT_METRICS:
                        result_lines.append(
                            f"RESULT,current,{pattern},{transport},{size},{metric},{_fmt_metric(metric_values[metric])}"
                        )
                else:
                    lines.append(f"      | {size}B | FAIL | FAIL | FAIL | FAIL | FAIL |")
                    failures.append(
                        f"{pattern} current {transport} {size}B: {reason or 'missing_result_lines'}"
                    )
            lines.append(f"    Testing {transport}: Done")
            lines.extend(multi_auto_hwm_lines(pattern, pattern_sizes[pattern]))
        lines.append("")

    lines.extend(_multi_effective_options(args, "result"))
    if result_lines:
        lines.append("## Result Data")
        lines.extend(result_lines)
    lines.extend(["", "## Completion"])
    lines.append(f"- success: {actual // len(REQUIRED_METRICS)}")
    lines.append(f"- unsupported: {sum(1 for status, _reason in cases.values() if status == 'unsupported')}")
    lines.append(f"- skip: {sum(1 for status, _reason in cases.values() if status == 'skip')}")
    lines.append(f"- fail: {len(failures)}")
    lines.append(f"- status: {'complete' if expected == actual else 'partial'}")
    lines.append(f"- expected_result_lines: {expected}")
    lines.append(f"- actual_result_lines: {actual}")
    if skips:
        lines.extend(["", "## Skips"])
        lines.extend(f"- {skip}" for skip in skips)
    if failures:
        lines.extend(["", "## Failures"])
        lines.extend(f"- {failure}" for failure in failures)
    status = "complete" if expected == actual else "partial"
    lines.extend(["", f"Saved result file: {args.report} (status={status})"])
    _write_report(lines, args.report, args.output)
    return 0 if expected == actual else 1


def render_skips(args):
    lines = ["## Skips"]
    with open(args.cases, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        for pattern, transport, size, status, reason in reader:
            if status == "skip":
                lines.append(f"- {pattern} {transport} {size}B: {reason}")
    text = "\n".join(lines) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(text)
    sys.stdout.write(text)
    return 0


def _read_result_rows_from_logs(tmp_dir):
    rows = defaultdict(lambda: defaultdict(list))
    for entry in os.listdir(tmp_dir):
        if not entry.endswith(".log"):
            continue
        path = os.path.join(tmp_dir, entry)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                parts = raw.strip().split(",")
                if len(parts) != 7 or parts[0] != "RESULT" or parts[1] != "current":
                    continue
                pattern, transport, size, metric, value = (
                    parts[2],
                    parts[3],
                    parts[4],
                    parts[5],
                    parts[6],
                )
                if metric not in REQUIRED_METRICS:
                    continue
                try:
                    rows[(pattern, transport, int(size))][metric].append(float(value))
                except ValueError:
                    continue
    return rows


def progress_case_row(args):
    metrics = {}
    with open(args.case_log, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            parts = raw.strip().split(",")
            if len(parts) != 7 or parts[0] != "RESULT" or parts[1] != "current":
                continue
            if parts[2] != args.pattern or parts[4] != args.size:
                continue
            metrics[parts[5]] = parts[6]

    fail_width = 16
    if args.suite == "single":
        missing_line = (
            f"      | {args.size + 'B':<8} | {'FAIL':>{fail_width}} | "
            f"{'FAIL':>12} | {'FAIL':>12} | {'FAIL':>12} | {'FAIL':>12} |"
        )
    else:
        missing_line = (
            f"      | {args.size + 'B':<8} | {'FAIL':>{fail_width}} | "
            f"{'FAIL':>12} | {'FAIL':>12} | {'FAIL':>12} | {'FAIL':>12} |"
        )
    if not all(metric in metrics for metric in REQUIRED_METRICS):
        print(missing_line)
        return 0

    values = {metric: float(metrics[metric]) for metric in REQUIRED_METRICS}
    size = int(args.size)
    if args.suite == "single":
        print(_single_case_row(args.pattern, size, values))
    else:
        print(_multi_case_row(args.pattern, size, values))
    return 0


def status_row(args):
    status = args.status.upper()
    print(
        f"      | {args.size + 'B':<8} | {status:>16} | {status:>12} | "
        f"{status:>12} | {status:>12} | {status:>12} |"
    )
    return 0


def render_log_tables(args):
    rows = _read_result_rows_from_logs(args.tmp_dir)
    by_pattern = defaultdict(list)
    for key, metric_values in rows.items():
        values = {}
        for metric in REQUIRED_METRICS:
            samples = metric_values.get(metric)
            if not samples:
                break
            values[metric] = _median(samples)
        if len(values) != len(REQUIRED_METRICS):
            continue
        pattern, transport, size = key
        by_pattern[pattern].append((transport, size, values))

    table_headers = SINGLE_TABLE_HEADER_LINES if args.suite == "single" else MULTI_TABLE_HEADER_LINES
    case_row = _single_case_row if args.suite == "single" else _multi_case_row
    direction = _single_direction if args.suite == "single" else _multi_direction
    lines = []
    for pattern_index, pattern in enumerate(sorted(by_pattern)):
        if pattern_index:
            lines.extend(["", "===============================================================================", ""])
        lines.append(f"## PATTERN: {pattern} ({direction(pattern)})")
        lines.append(f"  > Benchmarking current for {pattern}...")
        transports = defaultdict(list)
        for transport, size, values in by_pattern[pattern]:
            transports[transport].append((size, values))
        for transport in sorted(transports):
            lines.append(f"    Testing {transport}:")
            for header in table_headers:
                lines.append(f"      {header}")
            sizes = []
            for size, values in sorted(transports[transport]):
                if args.suite == "multi":
                    lines.append(f"    Testing {transport} | {size}B:")
                lines.append(case_row(pattern, size, values))
                sizes.append(size)
            lines.append(f"    Testing {transport}: Done")
            if args.suite == "multi":
                lines.extend(multi_auto_hwm_lines(pattern, sizes))
        lines.append("")

    sys.stdout.write("\n".join(lines))
    if lines:
        sys.stdout.write("\n")
    return 0


def add_common_report_args(parser):
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--cases", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--patterns", required=True)
    parser.add_argument("--transports", required=True)
    parser.add_argument("--msg-sizes", required=True)
    parser.add_argument("--runs", required=True)
    parser.add_argument("--duration", required=True)
    parser.add_argument("--results-tag", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--pin-cpu", default="")
    parser.add_argument("--hwm", default="")
    parser.add_argument("--send-hwm", default="")
    parser.add_argument("--recv-hwm", default="")
    parser.add_argument("--sndtimeo-ms", required=True)
    parser.add_argument("--rcvtimeo-ms", required=True)
    parser.add_argument("--elapsed-seconds", default="")
    parser.add_argument("--lang", required=True)
    parser.add_argument("--fail-fast", default="0")


def main(argv=None):
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    single = subparsers.add_parser("single-auto-hwm")
    single.add_argument("--patterns", required=True)
    single.add_argument("--msg-sizes", required=True)
    single.add_argument("--raw-results", default="")

    multi = subparsers.add_parser("multi-auto-hwm")
    multi.add_argument("--pattern", required=True)
    multi.add_argument("--msg-sizes", required=True)

    sort_data = subparsers.add_parser("sort-result-data")
    sort_data.add_argument("path")

    skips = subparsers.add_parser("render-skips")
    skips.add_argument("--cases", required=True)
    skips.add_argument("--output", default="")

    progress = subparsers.add_parser("progress-case-row")
    progress.add_argument("--suite", choices=("single", "multi"), required=True)
    progress.add_argument("--pattern", required=True)
    progress.add_argument("--size", required=True)
    progress.add_argument("--case-log", required=True)

    status = subparsers.add_parser("status-row")
    status.add_argument("--suite", choices=("single", "multi"), required=True)
    status.add_argument("--size", required=True)
    status.add_argument("--status", choices=("unsupported", "skip", "fail"), required=True)

    log_tables = subparsers.add_parser("render-log-tables")
    log_tables.add_argument("--suite", choices=("single", "multi"), required=True)
    log_tables.add_argument("--tmp-dir", required=True)

    render_single = subparsers.add_parser("render-single")
    add_common_report_args(render_single)
    render_single.add_argument("--io-threads", default="")

    render_multi = subparsers.add_parser("render-multi")
    add_common_report_args(render_multi)
    render_multi.add_argument("--clients", required=True)
    render_multi.add_argument("--common-io-threads", default="")
    render_multi.add_argument("--server-io-threads", default="")
    render_multi.add_argument("--client-io-threads", default="")
    render_multi.add_argument("--run-cooldown-ms", default="")
    render_multi.add_argument("--transport-transition-ms", required=True)
    render_multi.add_argument("--pattern-transition-ms", required=True)
    render_multi.add_argument("--connect-concurrency", default="128 (default)")

    args = parser.parse_args(argv)
    if args.command == "single-auto-hwm":
        for line in single_auto_hwm_detail_lines(
            parse_csv(args.patterns),
            parse_csv(args.msg_sizes),
            load_auto_hwm_detail_rows(args.raw_results),
        ):
            print(line)
        return
    if args.command == "multi-auto-hwm":
        for line in multi_auto_hwm_lines(args.pattern, parse_csv(args.msg_sizes)):
            print(line)
        return
    if args.command == "sort-result-data":
        with open(args.path, "r", encoding="utf-8", errors="replace") as fh:
            for line in sort_result_data_lines(fh):
                print(line)
        return
    if args.command == "render-skips":
        raise SystemExit(render_skips(args))
    if args.command == "progress-case-row":
        raise SystemExit(progress_case_row(args))
    if args.command == "status-row":
        raise SystemExit(status_row(args))
    if args.command == "render-log-tables":
        raise SystemExit(render_log_tables(args))
    if args.command == "render-single":
        raise SystemExit(render_single_report(args))
    if args.command == "render-multi":
        raise SystemExit(render_multi_report(args))


if __name__ == "__main__":
    main(sys.argv[1:])
