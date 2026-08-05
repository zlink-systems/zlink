import argparse
import os
import time
import statistics
import subprocess
import sys
from pathlib import Path

from perf_common import (
    build_report_path,
    parse_result_lines,
    pin_current_process_cpu0,
    render_effective_options,
    resolve_single_timeout_seconds,
    rows_by_case,
    status_row_text,
    throughput_unit,
)
from perf_report import SINGLE_TABLE_HEADER_LINES, single_auto_hwm_detail_lines
from perf_runtime import configure_runtime


ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parent.parent.parent.parent
DEFAULT_PYTHONPATH = ROOT.parent.parent / "src"
DEFAULT_PATTERNS = (
    "PAIR",
    "PUBSUB",
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "ROUTER_ROUTER",
)
DEFAULT_MSG_SIZES = ("64", "256", "1024", "65536", "131072", "262144")
RAW_TRANSPORTS = (
    ("tcp", "tls", "ws", "wss", "inproc")
    if sys.platform.startswith("win")
    else ("tcp", "tls", "ws", "wss", "inproc", "ipc")
)
POLICY_TRANSPORTS = {
    "PAIR": RAW_TRANSPORTS,
    "PUBSUB": RAW_TRANSPORTS,
    "DEALER_DEALER": RAW_TRANSPORTS,
    "DEALER_ROUTER": RAW_TRANSPORTS,
    "ROUTER_ROUTER": RAW_TRANSPORTS,
}
RUNNABLE_TRANSPORTS = POLICY_TRANSPORTS


def _require_binding_runtime():
    src_path = str(DEFAULT_PYTHONPATH.resolve())
    if src_path not in sys.path:
        sys.path.insert(0, src_path)
    import zlink

    try:
        zlink.version()
    except Exception as exc:
        raise SystemExit(
            "Python binding runtime is required for official perf runs. "
            "Run `python3 setup.py build_ext --inplace --force` in bindings/python."
        ) from exc


def parse_args(argv):
    parser = argparse.ArgumentParser(prog="run_benchmarks.sh")
    parser.add_argument("--pattern", default="ALL")
    parser.add_argument(
        "--duration",
        default=os.environ.get("PERF_SINGLE_DURATION_SECONDS", "5"),
    )
    parser.add_argument("--msg-sizes", default="")
    parser.add_argument("--transports", default="")
    parser.add_argument("--runs", default="1")
    parser.add_argument("--results-dir", default="")
    parser.add_argument("--results-tag", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--reuse-build", action="store_true")
    parser.add_argument("--clean-build", action="store_true")
    parser.add_argument("--io-threads", default="")
    parser.add_argument("--hwm", default="")
    parser.add_argument("--send-hwm", default="")
    parser.add_argument("--recv-hwm", default="")
    parser.add_argument("--buf", default="")
    parser.add_argument("--sndbuf", default="")
    parser.add_argument("--rcvbuf", default="")
    parser.add_argument("--sndtimeo", "--send-timeout-ms", dest="sndtimeo", default="")
    parser.add_argument("--rcvtimeo", "--recv-timeout-ms", dest="rcvtimeo", default="")
    parser.add_argument("--auto-hwm-profile", default="")
    parser.add_argument("--pin-cpu", action="store_true")
    return parser.parse_args(argv)


def _parse_csv(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def _parse_patterns(value):
    text = value.strip().upper()
    if text == "ALL":
        return list(DEFAULT_PATTERNS)
    patterns = [item.strip().upper() for item in text.split(",") if item.strip()]
    if not patterns:
        raise SystemExit("unsupported pattern: ")
    unknown = [pattern for pattern in patterns if pattern not in POLICY_TRANSPORTS]
    if unknown:
        raise SystemExit(f"unsupported pattern: {unknown[0]}")
    return patterns


def _parse_msg_sizes(args):
    source = args.msg_sizes or os.environ.get("PERF_MSG_SIZES", "")
    sizes = _parse_csv(source or ",".join(DEFAULT_MSG_SIZES))
    if not sizes:
        raise SystemExit("--msg-sizes must not be empty")
    return sizes


def _parse_transports(value):
    source = value or os.environ.get("PERF_TRANSPORTS", "")
    if not source:
        return None
    transports = [item.lower() for item in _parse_csv(source)]
    if not transports:
        raise SystemExit("--transports must not be empty")
    return transports


def _configure_core_runtime(env):
    return configure_runtime(env, REPO_ROOT)


def _transports_for_pattern(pattern, transports):
    if transports is None:
        return list(POLICY_TRANSPORTS[pattern])
    return list(transports)


def _grouped_option_text(patterns, value_for_pattern):
    groups = []
    for pattern in patterns:
        values = tuple(value_for_pattern(pattern))
        if groups and groups[-1][1] == values:
            groups[-1][0].append(pattern)
            continue
        groups.append(([pattern], values))
    if not groups:
        return ""
    if len(groups) == 1:
        return ",".join(groups[0][1])
    rendered = []
    for grouped_patterns, values in groups:
        rendered.append(f"{','.join(grouped_patterns)}={','.join(values)}")
    return "; ".join(rendered)


def _selected_configs(patterns, transports, msg_sizes):
    configs = []
    for pattern in patterns:
        for transport in _transports_for_pattern(pattern, transports):
            for msg_size in msg_sizes:
                configs.append((pattern, transport, msg_size))
    if not configs:
        raise SystemExit("no runnable pattern/transport combinations selected")
    return configs


def _parse_status_lines(output):
    rows = []
    for line in output.splitlines():
        if line.startswith("SKIP,") or line.startswith("UNSUPPORTED,"):
            rows.append(line.strip())
    return rows


def _grouped_case_metrics(output):
    return rows_by_case(parse_result_lines(output))


def _result_metrics_for_case(output, pattern, transport, msg_size):
    return _grouped_case_metrics(output).get((pattern, transport, str(msg_size)), {})


def _append_line(lines, line=""):
    print(line, flush=True)
    lines.append(line)


def _failure_reason(output):
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if not lines:
        return "no_data"
    return lines[-1]


def _status_kind(output):
    if parse_result_lines(output):
        return "ok"
    for line in _parse_status_lines(output):
        if line.startswith("UNSUPPORTED,"):
            return "unsupported"
        if line.startswith("SKIP,"):
            return "skip"
    return "fail"


def _metric_row(pattern, msg_size, metrics, *, indent="      "):
    throughput = f"{float(metrics.get('throughput', 0.0)) / 1000.0:7.2f} {throughput_unit(pattern)}"
    return (
        f"{indent}| {str(msg_size) + 'B':<8} | "
        f"{throughput:>16} | "
        f"{float(metrics.get('bandwidth', 0.0)):>8.2f} MB/s | "
        f"{float(metrics.get('latency', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p95', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p99', 0.0)):>9.3f} ms |"
    )


def pattern_direction(_pattern):
    return "one-way"


def _status_row(msg_size, status, *, indent="      "):
    return indent + status_row_text(int(msg_size), status)


def _median_metrics(outputs, pattern, transport, msg_size):
    metrics_list = [
        _result_metrics_for_case(output, pattern, transport, msg_size)
        for output in outputs
    ]
    metrics_list = [metrics for metrics in metrics_list if metrics]
    if not metrics_list:
        return {}
    medians = {}
    for metric in ("throughput", "bandwidth", "latency", "latency_p95", "latency_p99"):
        values = [float(metrics[metric]) for metrics in metrics_list if metric in metrics]
        if values:
            medians[metric] = statistics.median(values)
    return medians


def _run_pattern(args, env, pattern, transport, msg_size):
    if transport == "ipc" and sys.platform.startswith("win"):
        return f"SKIP,current,{pattern},{transport},windows_ipc_unsupported"
    if transport not in RUNNABLE_TRANSPORTS.get(pattern, ()):
        return f"UNSUPPORTED,current,{pattern},{transport}"
    entry = ROOT / f"perf_{pattern.lower()}.py"
    if not entry.exists():
        raise SystemExit(f"unsupported pattern: {pattern}")
    cmd = [
        sys.executable,
        str(entry),
        "--transport",
        transport,
        "--duration",
        args.duration,
        "--msg-size",
        msg_size,
    ]
    timeout_s = resolve_single_timeout_seconds(float(args.duration))
    result = subprocess.run(
        cmd,
        cwd=str(ROOT.parent.parent),
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout_s,
    )
    stdout_text = result.stdout.strip() if result.stdout else ""
    stderr_text = result.stderr.strip() if result.stderr else ""
    if result.returncode == 0:
        return stdout_text
    failure_text = stderr_text or stdout_text
    raise SystemExit(
        failure_text or f"{pattern} {transport} failed with exit code {result.returncode}"
    )


def _build_options(args, patterns, transports, msg_sizes):
    hwm = args.hwm or "auto-hwm"
    sndhwm = args.send_hwm or args.hwm or "auto-hwm"
    rcvhwm = args.recv_hwm or args.hwm or "auto-hwm"
    sndbuf = args.sndbuf or args.buf or "-1"
    rcvbuf = args.rcvbuf or args.buf or "-1"
    return {
        "lang": "python",
        "suite": "single",
        "runs": args.runs,
        "duration_seconds": args.duration,
        "timeout_seconds": resolve_single_timeout_seconds(float(args.duration)),
        "io_threads": args.io_threads or "1",
        "hwm": hwm,
        "sndhwm": sndhwm,
        "rcvhwm": rcvhwm,
        "sndbuf": sndbuf,
        "rcvbuf": rcvbuf,
        "sndtimeo_ms": args.sndtimeo or os.environ.get("PERF_SINGLE_SNDTIMEO_MS", "200"),
        "rcvtimeo_ms": args.rcvtimeo or os.environ.get("PERF_SINGLE_RCVTIMEO_MS", "200"),
        "ctx_auto_hwm_enable": os.environ.get("PERF_CTX_AUTO_HWM_ENABLE", "core-default"),
        "ctx_auto_hwm_profile": args.auto_hwm_profile
        or os.environ.get("PERF_SINGLE_CTX_AUTO_HWM_PROFILE")
        or os.environ.get("PERF_CTX_AUTO_HWM_PROFILE", "balanced"),
        "patterns": ",".join(patterns),
        "transports": _grouped_option_text(patterns, lambda pattern: _transports_for_pattern(pattern, transports)),
        "msg_sizes": ",".join(msg_sizes),
        "smoke": "1" if args.smoke else "0",
    }


def main(argv=None):
    start_time = time.perf_counter()
    args = parse_args(argv or sys.argv[1:])
    _require_binding_runtime()
    if args.pin_cpu and not pin_current_process_cpu0():
        print("warning: cpu pinning requested but could not pin to cpu 0", file=sys.stderr)
    patterns = _parse_patterns(args.pattern)
    transports = _parse_transports(args.transports)
    msg_sizes = _parse_msg_sizes(args)
    runs = int(args.runs)
    if runs <= 0:
        raise SystemExit("--runs must be > 0")
    configs = _selected_configs(patterns, transports, msg_sizes)

    env = dict(os.environ)
    env["PYTHONPATH"] = str(DEFAULT_PYTHONPATH.resolve())
    runtime_info = _configure_core_runtime(env)
    if args.io_threads:
        env["PERF_IO_THREADS"] = args.io_threads
    if args.hwm:
        env["PERF_SINGLE_HWM"] = args.hwm
    if args.send_hwm:
        env["PERF_SINGLE_SNDHWM"] = args.send_hwm
    if args.recv_hwm:
        env["PERF_SINGLE_RCVHWM"] = args.recv_hwm
    if args.buf:
        env["PERF_SINGLE_SNDBUF"] = args.buf
        env["PERF_SINGLE_RCVBUF"] = args.buf
    if args.sndbuf:
        env["PERF_SINGLE_SNDBUF"] = args.sndbuf
    if args.rcvbuf:
        env["PERF_SINGLE_RCVBUF"] = args.rcvbuf
    if args.sndtimeo:
        env["PERF_SINGLE_SNDTIMEO_MS"] = args.sndtimeo
    if args.rcvtimeo:
        env["PERF_SINGLE_RCVTIMEO_MS"] = args.rcvtimeo
    if args.auto_hwm_profile:
        env["PERF_CTX_AUTO_HWM_PROFILE"] = args.auto_hwm_profile

    options = _build_options(args, patterns, transports, msg_sizes)
    fail_fast = os.environ.get("PERF_FAIL_FAST", "0") == "1"
    options["fail_fast"] = "1" if fail_fast else "0"
    sections = []
    emitted_chunks = []
    status_lines = []
    failures = []
    fail_count = 0
    stop_early = False
    case_ordinal = 1

    _append_line(sections, f"META,runtime_libzlink,{runtime_info.path}")
    _append_line(sections, f"META,runtime_libzlink_sha256,{runtime_info.sha256}")
    _append_line(sections)
    _append_line(sections, render_effective_options(options))

    for pattern in patterns:
        if stop_early:
            break
        if pattern != patterns[0]:
            _append_line(sections, "===============================================================================")
            _append_line(sections)
        _append_line(sections, f"## PATTERN: {pattern} ({pattern_direction(pattern)})")
        _append_line(sections, f"  > Benchmarking current for {pattern}...")
        pattern_transports = _transports_for_pattern(pattern, transports)
        for transport in pattern_transports:
            if stop_early:
                break
            _append_line(sections, f"    Testing {transport}:")
            if runs == 1:
                for header_line in SINGLE_TABLE_HEADER_LINES:
                    _append_line(sections, f"      {header_line}")
                for msg_size in msg_sizes:
                    case_env = dict(env)
                    case_env["PERF_RUN_ID"] = str(case_ordinal)
                    case_ordinal += 1
                    try:
                        output = _run_pattern(args, case_env, pattern, transport, msg_size)
                    except SystemExit as exc:
                        output = str(exc).strip()
                    status_kind = _status_kind(output)
                    if status_kind == "fail":
                        fail_count += 1
                        if fail_fast:
                            stop_early = True
                    if output:
                        emitted_chunks.append(output)
                        status_lines.extend(_parse_status_lines(output))
                    metrics = _result_metrics_for_case(output, pattern, transport, msg_size)
                    if metrics:
                        _append_line(sections, _metric_row(pattern, msg_size, metrics))
                    elif status_kind == "unsupported":
                        _append_line(sections, _status_row(msg_size, "UNSUPPORTED"))
                    else:
                        if status_kind == "fail":
                            failures.append(
                                f"- {pattern} current {transport} {msg_size}B: {_failure_reason(output)}"
                            )
                        _append_line(sections, _status_row(msg_size, "FAIL"))
                    if stop_early:
                        break
                suffix = f"(failures={fail_count}) Done" if fail_count else "Done"
                _append_line(sections, f"    Testing {transport}: {suffix}")
            else:
                transport_failures = 0
                run_outputs = {msg_size: [] for msg_size in msg_sizes}
                for run_index in range(runs):
                    if stop_early:
                        break
                    _append_line(sections, f"      run {run_index + 1}/{runs}:")
                    for header_line in SINGLE_TABLE_HEADER_LINES:
                        _append_line(sections, f"        {header_line}")
                    for msg_size in msg_sizes:
                        case_env = dict(env)
                        case_env["PERF_RUN_ID"] = str(case_ordinal)
                        case_ordinal += 1
                        try:
                            output = _run_pattern(args, case_env, pattern, transport, msg_size)
                        except SystemExit as exc:
                            output = str(exc).strip()
                        status_kind = _status_kind(output)
                        if status_kind == "fail":
                            fail_count += 1
                            transport_failures += 1
                            if fail_fast:
                                stop_early = True
                        if output:
                            emitted_chunks.append(output)
                            status_lines.extend(_parse_status_lines(output))
                            run_outputs[msg_size].append(output)
                        metrics = _result_metrics_for_case(output, pattern, transport, msg_size)
                        if metrics:
                            _append_line(
                                sections,
                                _metric_row(pattern, msg_size, metrics, indent="        "),
                            )
                        elif status_kind == "unsupported":
                            _append_line(
                                sections,
                                _status_row(msg_size, "UNSUPPORTED", indent="        "),
                            )
                        else:
                            if status_kind == "fail":
                                failures.append(
                                    f"- {pattern} current {transport} {msg_size}B: {_failure_reason(output)}"
                                )
                            _append_line(
                                sections,
                                _status_row(msg_size, "FAIL", indent="        "),
                            )
                        if stop_early:
                            break
                _append_line(sections, "      median:")
                for header_line in SINGLE_TABLE_HEADER_LINES:
                    _append_line(sections, f"        {header_line}")
                for msg_size in msg_sizes:
                    metrics = _median_metrics(
                        run_outputs[msg_size], pattern, transport, msg_size
                    )
                    if metrics:
                        _append_line(
                            sections,
                            _metric_row(pattern, msg_size, metrics, indent="        "),
                        )
                    elif any(
                        output.startswith("UNSUPPORTED,")
                        for output in run_outputs[msg_size]
                    ):
                        _append_line(
                            sections,
                            _status_row(msg_size, "UNSUPPORTED", indent="        "),
                        )
                    else:
                        _append_line(
                            sections,
                            _status_row(msg_size, "FAIL", indent="        "),
                        )
                suffix = (
                    f"(failures={transport_failures}) Done" if transport_failures else "Done"
                )
                _append_line(sections, f"    Testing {transport}: {suffix}")
        _append_line(sections)

    rows = parse_result_lines("\n".join(emitted_chunks))
    emitted_result_lines = [
        line
        for chunk in emitted_chunks
        for line in chunk.splitlines()
        if line.startswith("RESULT,")
    ]
    skipped_cases = 0
    unsupported_cases = 0
    for line in status_lines:
        if line.startswith("SKIP,"):
            skipped_cases += 1
        elif line.startswith("UNSUPPORTED,"):
            unsupported_cases += 1
    expected_cases = max(0, len(configs) * runs - skipped_cases - unsupported_cases)
    expected_result_lines = expected_cases * 5
    status = "complete" if len(rows) == expected_result_lines else "partial"

    if failures:
        _append_line(sections)
        _append_line(sections, "## Failures")
        for line in failures:
            _append_line(sections, line)
        _append_line(sections)

    for line in single_auto_hwm_detail_lines(patterns, msg_sizes):
        _append_line(sections, line)
    _append_line(sections)
    _append_line(sections, render_effective_options(options, section="result"))
    if emitted_result_lines:
        _append_line(sections)
        _append_line(sections, "## Result Data")
        for line in emitted_result_lines:
            _append_line(sections, line)
    _append_line(sections)
    _append_line(sections, "## Completion")
    _append_line(sections, f"- status: {status}")
    _append_line(sections, f"- expected_result_lines: {expected_result_lines}")
    _append_line(sections, f"- actual_result_lines: {len(rows)}")
    if args.smoke:
        _append_line(sections)
        _append_line(sections, f"Smoke completion: status={status}")
        elapsed = max(0, int(time.perf_counter() - start_time))
        print(
            f"Total benchmark time: {elapsed}s ({elapsed}s, exit={0 if status == 'complete' else 1})",
            flush=True,
        )
        if status != "complete":
            raise SystemExit(1)
        return

    report_path = build_report_path(
        lang="python",
        suite="single",
        results_dir=args.results_dir or None,
        tag=args.results_tag or None,
    )
    _append_line(sections)
    _append_line(sections, f"Saved result file: {report_path} (status={status})")
    final_output = "\n".join(sections).rstrip() + "\n"

    report_path.write_text(final_output, encoding="utf-8")
    if args.output:
        Path(args.output).write_text(final_output, encoding="utf-8")
    elapsed = max(0, int(time.perf_counter() - start_time))
    print(f"Total benchmark time: {elapsed}s ({elapsed}s, exit={0 if status == 'complete' else 1})", flush=True)
    if status != "complete":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
