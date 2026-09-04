#!/usr/bin/env python3
"""Single-pattern dotnet benchmark emitter.

This emitter is a byte-faithful port of
``bindings/c/perf/single/run_comparison.py`` reporting. The ONLY intended
differences vs the C canonical single report are:

  * ``- lang: dotnet`` (C reports ``- lang: c``); the lang identifier is
    inherently binding-specific.
  * measured numeric values (throughput/bandwidth/latency) are produced by
    the dotnet benchmark binary, not the C benchmark binary.

Every other byte (leading blank line, ``## Effective Options`` keys/order,
``## PATTERN`` headers, ``Testing``/``Done``/cooldown lines, table header /
separator / column widths / decimals / units, ``=====`` pattern separators,
``## Auto-HWM Detail`` tables, ``## Result Data`` ``RESULT,current,`` lines
with ``.3f`` formatting, ``## Completion`` block and the trailing
``Saved result file:`` line) matches the C canonical single emitter exactly.

The dotnet benchmark binary is invoked exactly like the C single benchmark:
one process per (pattern, transport, size, run). All prior dotnet perf fixes
(8 audit fixes + C-faithful blocking-first-recv / DONTWAIT burst-drain /
in-flight credit cap) live inside the benchmark .cs sources and are NOT
touched by this presentation-only emitter.
"""

from __future__ import annotations

import argparse
import datetime
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple

IS_WINDOWS = os.name == "nt"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""

SCRIPT_DIR = os.path.abspath(os.path.dirname(__file__))
PERF_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))

DEFAULT_PATTERNS = [
    "PAIR",
    "PUBSUB",
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "DEALER_ROUTER_REQREP",
    "ROUTER_ROUTER",
    "ROUTER_ROUTER_REQREP",
]

PATTERN_TO_BINARY = {
    "PAIR": "Zlink.BindingBench",
    "PUBSUB": "Zlink.BindingBench",
    "DEALER_DEALER": "Zlink.BindingBench",
    "DEALER_ROUTER": "Zlink.BindingBench",
    "DEALER_ROUTER_REQREP": "Zlink.BindingBench",
    "ROUTER_ROUTER": "Zlink.BindingBench",
    "ROUTER_ROUTER_REQREP": "Zlink.BindingBench",
}

DEFAULT_MSG_SIZES_STANDARD = [64, 256, 1024, 65536, 131072, 262144]
DEFAULT_MSG_SIZES_STREAM = [64, 256, 1024, 65536]
DEFAULT_SOCKET_TRANSPORTS = ["tcp", "tls", "ws", "wss", "inproc"]
if not IS_WINDOWS:
    DEFAULT_SOCKET_TRANSPORTS.append("ipc")
STREAM_TRANSPORT_PATTERNS = set()
STREAM_SIZE_PATTERNS = set()

DEFAULT_RESULTS_DIR = os.path.join(PERF_DIR, "results")
DEFAULT_MAX_RESULT_FILES = 100
# Result Data lines stay byte-identical to the C canonical emitter, which
# always tags rows as ``current`` regardless of binding.
RESULT_DATA_TAG = "current"
# The dotnet benchmark binary emits ``RESULT,dotnet,...`` metric lines.
BENCH_LIB_TAG = "dotnet"
RESULT_LANG = "dotnet"
RESULT_SUITE = "single"
LATENCY_P95_METRIC = "latency_p95"
LATENCY_P99_METRIC = "latency_p99"
REQUIRED_RESULT_METRICS = (
    "throughput",
    "bandwidth",
    "latency",
    LATENCY_P95_METRIC,
    LATENCY_P99_METRIC,
)
REQUIRED_RESULT_METRIC_COUNT = len(REQUIRED_RESULT_METRICS)
PATTERN_SEPARATOR = "==============================================================================="


@dataclass
class RunOutcome:
    status: str  # success | unsupported | skip | fail
    throughput: float = 0.0
    bandwidth: float = 0.0
    latency: float = 0.0
    latency_p95: float = 0.0
    latency_p99: float = 0.0
    reason: str = ""
    warnings: Optional[List[str]] = None
    stderr: str = ""
    auto_hwm_details: Optional[List[Dict[str, str]]] = None


@dataclass
class ComboRecord:
    status: str  # success | unsupported | skip | fail
    throughput: float = 0.0
    bandwidth: float = 0.0
    latency: float = 0.0
    latency_p95: float = 0.0
    latency_p99: float = 0.0


class TeeStream:
    def __init__(self, *streams):
        self._streams = streams

    def write(self, data: str) -> int:
        for stream in self._streams:
            stream.write(data)
            stream.flush()
        return len(data)

    def flush(self) -> None:
        for stream in self._streams:
            stream.flush()


def env_get(name: str) -> str:
    val = os.environ.get(name)
    if val:
        return val
    return ""


def env_flag_enabled(name: str) -> bool:
    return env_get(name) == "1"


def build_meta_items(runs: int) -> List[Tuple[str, str]]:
    try:
        commit = subprocess.check_output(
            ["git", "-C", REPO_ROOT, "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        commit = "unknown"
    return [
        ("os", f"{platform.system()} {platform.release()}".strip()),
        ("cpu", platform.processor().strip() or "unknown"),
        ("cores", str(os.cpu_count() or 0)),
        ("build", env_get("PERF_CONFIGURATION") or "Release"),
        ("commit", commit),
        ("core_source", env_get("PERF_CORE_SOURCE") or "unknown"),
        ("core_version", env_get("PERF_CORE_VERSION") or "unknown"),
        ("core_runtime", env_get("PERF_CORE_RUNTIME") or "unknown"),
        ("timestamp", datetime.datetime.now().astimezone().isoformat(timespec="seconds")),
        ("runs", str(runs)),
    ]


def parse_env_list(name: str, cast_fn):
    val = env_get(name)
    if not val:
        return None
    items = []
    for part in val.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            items.append(cast_fn(part))
        except ValueError:
            continue
    return items or None


def parse_env_int(name: str, default: int) -> int:
    val = env_get(name)
    if not val:
        return default
    try:
        return int(val)
    except ValueError:
        return default


def resolve_latency_triplet(
    latency: Optional[float],
    latency_p95: Optional[float],
    latency_p99: Optional[float],
) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    mean = latency
    p95 = latency_p95 if latency_p95 is not None else mean
    if p95 is None:
        p95 = mean
    p99 = latency_p99 if latency_p99 is not None else p95
    if p99 is None:
        p99 = p95 if p95 is not None else mean
    return mean, p95, p99


def run_command_with_metrics(
    cmd: List[str], env: Dict[str, str], timeout_sec: int
) -> Dict[str, object]:
    started = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    timed_out = False
    try:
        while True:
            rc = proc.poll()
            if rc is not None:
                break
            if (time.monotonic() - started) > timeout_sec:
                timed_out = True
                proc.kill()
                break
            time.sleep(0.02)

        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            timed_out = True
    finally:
        if proc.poll() is None:
            proc.kill()
            stdout, stderr = proc.communicate()
            timed_out = True

    return {
        "returncode": proc.returncode,
        "stdout": stdout or "",
        "stderr": stderr or "",
        "timed_out": timed_out,
    }


FAIL_FAST = env_flag_enabled("PERF_FAIL_FAST")


def platform_tag() -> str:
    if IS_WINDOWS:
        return "windows"
    if "darwin" in platform.system().lower():
        return "macos"
    return "linux"


def sanitize_suffix(value: str) -> str:
    if not value:
        return ""
    return re.sub(r"[^a-zA-Z0-9._-]+", "_", value.strip())


def build_result_filename(tag: str = "") -> str:
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    name = f"perf_{RESULT_LANG}_{RESULT_SUITE}_{platform_tag()}_{stamp}"
    clean_tag = sanitize_suffix(tag)
    if clean_tag:
        name = f"{name}_{clean_tag}"
    return f"{name}.txt"


def single_result_dir(results_root: str) -> str:
    return os.path.join(results_root, "single", "report")


def enforce_file_retention(report_dir: str) -> None:
    try:
        files = sorted(
            (
                p
                for p in os.listdir(report_dir)
                if os.path.isfile(os.path.join(report_dir, p))
            )
        )
    except OSError:
        return
    overflow = len(files) - DEFAULT_MAX_RESULT_FILES
    for name in files[: max(0, overflow)]:
        try:
            os.unlink(os.path.join(report_dir, name))
        except OSError:
            pass


def select_transports(pattern: str) -> List[str]:
    base = DEFAULT_SOCKET_TRANSPORTS
    env_transports = parse_env_list("PERF_TRANSPORTS", str)
    if not env_transports:
        return list(base)
    return [t for t in base if t in env_transports]


def default_msg_sizes_for_pattern(pattern: str) -> List[int]:
    if pattern in STREAM_SIZE_PATTERNS:
        return list(DEFAULT_MSG_SIZES_STREAM)
    return list(DEFAULT_MSG_SIZES_STANDARD)


def msg_sizes_for_pattern(pattern: str) -> List[int]:
    env_sizes = parse_env_list("PERF_MSG_SIZES", int)
    if env_sizes:
        return env_sizes
    return default_msg_sizes_for_pattern(pattern)


def parse_auto_hwm_detail_line(line: str) -> Optional[Dict[str, str]]:
    stripped = (line or "").strip()
    if not stripped.startswith("AUTO_HWM_DETAIL,"):
        return None
    fields: Dict[str, str] = {}
    for item in stripped.split(",")[1:]:
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields if fields else None


def bytes_to_kb_display(value: str) -> str:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return "?"
    if parsed <= 0:
        return "0"
    if parsed % 1024 == 0:
        return str(parsed // 1024)
    return f"{parsed / 1024.0:.1f}"


def auto_hwm_detail_cell_widths(
    rows: List[Dict[str, str]],
    columns: Tuple[Tuple[str, str], ...],
) -> List[int]:
    widths: List[int] = []
    for header, key in columns:
        width = len(header)
        for row in rows:
            width = max(width, len(str(row.get(key, "?"))))
        widths.append(width)
    return widths


def emit_auto_hwm_detail_table(
    rows: List[Dict[str, str]],
    pattern: str,
) -> bool:
    pattern_rows = [
        row
        for row in rows
        if row.get("pattern", "").upper() == pattern.upper()
    ]
    if not pattern_rows:
        return False

    display_rows: List[Dict[str, str]] = []
    seen = set()
    for row in pattern_rows:
        display = dict(row)
        display["sndbuf_kb"] = bytes_to_kb_display(row.get("effective_sndbuf", ""))
        display["rcvbuf_kb"] = bytes_to_kb_display(row.get("effective_rcvbuf", ""))
        key = tuple(
            display.get(name, "")
            for name in (
                "msg_size",
                "component",
                "owner",
                "socket",
                "socket_type",
                "role",
                "sndhwm",
                "rcvhwm",
                "sndbuf_kb",
                "rcvbuf_kb",
                "effective_message_bytes",
                "socket_message_slots",
            )
        )
        if key in seen:
            continue
        seen.add(key)
        display_rows.append(display)

    if not display_rows:
        return False

    display_rows.sort(
        key=lambda row: (
            int(row.get("msg_size", "0") or "0"),
            row.get("component", ""),
            row.get("owner", ""),
            row.get("socket", ""),
        )
    )
    columns = (
        ("Size(B)", "msg_size"),
        ("Component", "component"),
        ("Owner", "owner"),
        ("Socket", "socket"),
        ("Type", "socket_type"),
        ("Role", "role"),
        ("SNDHWM", "sndhwm"),
        ("RCVHWM", "rcvhwm"),
        ("SNDBUF(KB)", "sndbuf_kb"),
        ("RCVBUF(KB)", "rcvbuf_kb"),
        ("MsgUnit(B)", "effective_message_bytes"),
        ("Slots", "socket_message_slots"),
    )
    widths = auto_hwm_detail_cell_widths(display_rows, columns)

    print("\n## Auto-HWM Detail")
    print(f"- pattern: {pattern}")
    header = "| " + " | ".join(
        f"{columns[index][0]:<{widths[index]}}"
        for index in range(len(columns))
    ) + " |"
    separator = "|-" + "-|-".join("-" * width for width in widths) + "-|"
    print(header)
    print(separator)
    for row in display_rows:
        print(
            "| "
            + " | ".join(
                f"{str(row.get(columns[index][1], '?')):<{widths[index]}}"
                for index in range(len(columns))
            )
            + " |"
        )
    return True


def emit_result_lines(combo_results: Dict[Tuple[str, str, int], ComboRecord]) -> None:
    for key in sorted(combo_results.keys()):
        pattern, transport, size = key
        record = combo_results[key]
        if record.status != "success":
            continue

        metrics = [
            ("throughput", record.throughput),
            ("bandwidth", record.bandwidth),
            ("latency", record.latency),
            (LATENCY_P95_METRIC, record.latency_p95),
            (LATENCY_P99_METRIC, record.latency_p99),
        ]

        for metric_name, value in metrics:
            print(
                f"RESULT,{RESULT_DATA_TAG},{pattern},{transport},{size},"
                f"{metric_name},{value:.3f}"
            )


def single_table_header_line() -> str:
    return (
        "| Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |"
        "   Lat.P95(ms) |   Lat.P99(ms) |"
    )


def single_table_separator_line() -> str:
    return (
        "|----------|------------------|--------------|--------------|--------------|"
        "--------------|"
    )


def format_throughput(pattern: str, throughput_per_sec: float) -> str:
    unit = "Kops/s" if pattern.endswith("_REQREP") else "Kmsg/s"
    return f"{throughput_per_sec/1e3:6.2f} {unit}"


def format_bandwidth(bandwidth_mb_s: float) -> str:
    return f"{bandwidth_mb_s:8.2f} MB/s"


def format_latency_ms(latency_ms: float) -> str:
    return f"{latency_ms:8.3f} ms"


def single_table_row_line(
    size: int,
    status: str,
    record: Optional[ComboRecord] = None,
    pattern: str = "",
) -> str:
    if status == "success" and record is not None:
        _, lat_p95, lat_p99 = resolve_latency_triplet(
            record.latency,
            record.latency_p95,
            record.latency_p99,
        )
        tp_s = format_throughput(pattern, record.throughput)
        bw_s = format_bandwidth(record.bandwidth)
        lat_s = format_latency_ms(record.latency)
        lat95_s = format_latency_ms(lat_p95 if lat_p95 is not None else 0.0)
        lat99_s = format_latency_ms(lat_p99 if lat_p99 is not None else 0.0)
    elif status == "unsupported":
        tp_s = "UNSUPPORTED"
        bw_s = "UNSUPPORTED"
        lat_s = "UNSUPPORTED"
        lat95_s = "UNSUPPORTED"
        lat99_s = "UNSUPPORTED"
    else:
        tp_s = "FAIL"
        bw_s = "FAIL"
        lat_s = "FAIL"
        lat95_s = "FAIL"
        lat99_s = "FAIL"
    return (
        f"| {f'{size}B':<8} | {tp_s:>16} | {bw_s:>12} | {lat_s:>12} | "
        f"{lat95_s:>12} | {lat99_s:>12} |"
    )


def pattern_direction_label(pattern: str) -> str:
    if pattern.endswith("_REQREP"):
        return "request/reply"
    return "one-way"


def parse_metric_from_result_line(
    line: str,
    expected_lib: str,
    expected_pattern: str,
    expected_transport: str,
    expected_size: int,
) -> Tuple[Optional[Tuple[str, float]], Optional[str]]:
    if not line.startswith("RESULT,"):
        return None, None
    parts = line.split(",")
    if len(parts) != 7:
        return None, f"ignored malformed RESULT line (field_count={len(parts)}): {line}"
    try:
        lib = parts[1].strip()
        pattern = parts[2].strip().upper()
        transport = parts[3].strip()
        size = int(parts[4].strip())
        metric = parts[5].strip().lower()
        value = float(parts[6].strip())
    except ValueError:
        return None, f"ignored malformed RESULT numeric field: {line}"
    if lib != expected_lib or pattern != expected_pattern.upper():
        return None, None
    if transport != expected_transport or size != expected_size:
        return None, None
    if metric not in (
        "throughput",
        "bandwidth",
        "latency",
        LATENCY_P95_METRIC,
        LATENCY_P99_METRIC,
    ):
        return None, f"ignored unknown RESULT metric '{metric}': {line}"
    return (metric, value), None


def detect_special_status(
    stdout: str,
    expected_lib: str,
    expected_pattern: str,
    expected_transport: str,
) -> Optional[str]:
    expected_pattern = expected_pattern.upper()
    expected_transport = expected_transport.lower()
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("UNSUPPORTED,"):
            parts = line.split(",")
            if len(parts) >= 4:
                lib = parts[1].strip()
                pattern = parts[2].strip().upper()
                transport = parts[3].strip().lower()
                if (
                    lib == expected_lib
                    and pattern == expected_pattern
                    and transport == expected_transport
                ):
                    return "unsupported"
        elif line.startswith("SKIP,"):
            parts = line.split(",")
            if len(parts) >= 5:
                lib = parts[1].strip()
                pattern = parts[2].strip().upper()
                transport = parts[3].strip().lower()
                if (
                    lib == expected_lib
                    and pattern == expected_pattern
                    and transport == expected_transport
                ):
                    return "skip"
    return None


def build_bench_cmd(binary_path: str, args: List[str], pin_cpu: bool) -> List[str]:
    if binary_path.lower().endswith(".dll"):
        prefix = ["dotnet", binary_path]
    else:
        prefix = [binary_path]
    if IS_WINDOWS:
        return prefix + list(args)
    if (
        pin_cpu
        or env_flag_enabled("PERF_TASKSET")
        or os.environ.get("PERF_TASKSET") == "1"
    ):
        return ["taskset", "-c", "1"] + prefix + list(args)
    return prefix + list(args)


def resolve_dotnet_binary() -> str:
    configuration = env_get("PERF_CONFIGURATION") or "Release"
    base = os.path.join(
        SCRIPT_DIR,
        "Zlink.BindingBench",
        "bin",
        configuration,
        "net8.0",
    )
    exe = os.path.join(base, "Zlink.BindingBench" + EXE_SUFFIX)
    if os.path.isfile(exe) and os.access(exe, os.X_OK):
        return exe
    dll = os.path.join(base, "Zlink.BindingBench.dll")
    if os.path.isfile(dll):
        return dll
    return exe


def get_bench_env() -> Dict[str, str]:
    env = os.environ.copy()
    # Mirror the prior dotnet runner JIT/runtime tuning so measured values
    # stay consistent with the established dotnet perf baseline.
    env.setdefault("DOTNET_TieredCompilation", "1")
    env.setdefault("DOTNET_TC_QuickJitForLoops", "1")
    env.setdefault("DOTNET_ReadyToRun", "1")
    return env


def run_single_test(
    binary_path: str,
    pattern: str,
    transport: str,
    size: int,
    timeout_sec: int,
    pin_cpu: bool,
) -> RunOutcome:
    cmd = build_bench_cmd(binary_path, [pattern, transport, str(size)], pin_cpu)
    env = get_bench_env()
    try:
        sampled = run_command_with_metrics(cmd, env, timeout_sec)
    except Exception as exc:  # pragma: no cover - defensive
        return RunOutcome(status="fail", reason=f"exception:{exc}")

    stdout = str(sampled.get("stdout") or "")
    stderr = str(sampled.get("stderr") or "")

    metrics: Dict[str, float] = {}
    warnings: List[str] = []
    auto_hwm_details: List[Dict[str, str]] = []
    for line in stdout.splitlines():
        auto_hwm_detail = parse_auto_hwm_detail_line(line)
        if auto_hwm_detail:
            auto_hwm_details.append(auto_hwm_detail)
            continue
        parsed, warning = parse_metric_from_result_line(
            line, BENCH_LIB_TAG, pattern, transport, size
        )
        if warning:
            warnings.append(warning)
        if not parsed:
            continue
        metric, value = parsed
        if metric in metrics:
            warnings.append(
                "duplicate RESULT metric detected; keeping last value: "
                f"{pattern} {transport} {size}B {metric}"
            )
        metrics[metric] = value

    if sampled.get("timed_out"):
        return RunOutcome(
            status="fail",
            reason="timeout",
            warnings=warnings or None,
            stderr=stderr,
        )

    special = detect_special_status(stdout, BENCH_LIB_TAG, pattern, transport)
    return_code = int(sampled.get("returncode") or 0)
    if return_code != 0:
        return RunOutcome(
            status="fail",
            reason=f"non_zero_exit_{return_code}",
            warnings=warnings or None,
            stderr=stderr,
        )

    if "throughput" in metrics and "bandwidth" in metrics and "latency" in metrics:
        latency_mean, latency_p95, latency_p99 = resolve_latency_triplet(
            metrics.get("latency"),
            metrics.get(LATENCY_P95_METRIC),
            metrics.get(LATENCY_P99_METRIC),
        )
        latency_mean = (
            latency_mean if latency_mean is not None else metrics["latency"]
        )
        latency_p95 = (
            latency_p95 if latency_p95 is not None else metrics["latency"]
        )
        latency_p99 = (
            latency_p99 if latency_p99 is not None else metrics["latency"]
        )
        observed = (
            metrics["throughput"], metrics["bandwidth"], latency_mean,
            latency_p95, latency_p99,
        )
        if (not all(math.isfinite(value) for value in observed)
                or metrics["throughput"] <= 0
                or metrics["bandwidth"] <= 0
                or any(value < 0 for value in observed[2:])):
            return RunOutcome(
                status="fail",
                reason="invalid_metrics",
                warnings=warnings or None,
                stderr=stderr,
            )
        return RunOutcome(
            status="success",
            throughput=metrics["throughput"],
            bandwidth=metrics["bandwidth"],
            latency=latency_mean,
            latency_p95=latency_p95,
            latency_p99=latency_p99,
            warnings=warnings or None,
            stderr=stderr,
            auto_hwm_details=auto_hwm_details or None,
        )

    if special == "unsupported" and not metrics:
        return RunOutcome(
            status="unsupported",
            reason="unsupported",
            warnings=warnings or None,
            stderr=stderr,
        )
    if special == "skip" and not metrics:
        return RunOutcome(
            status="skip",
            reason="skip",
            warnings=warnings or None,
            stderr=stderr,
        )

    return RunOutcome(
        status="fail",
        reason="no_data",
        warnings=warnings or None,
        stderr=stderr,
    )


def parse_pattern_arg(pattern_arg: str) -> List[str]:
    if pattern_arg.upper() == "ALL":
        return list(DEFAULT_PATTERNS)

    requested = []
    for part in pattern_arg.split(","):
        p = part.strip().upper()
        if not p:
            continue
        requested.append(p)

    if not requested:
        raise ValueError("--pattern requires at least one value")

    unknown = [p for p in requested if p not in PATTERN_TO_BINARY]
    if unknown:
        raise ValueError("unsupported patterns: " + ", ".join(sorted(set(unknown))))

    ordered = [p for p in DEFAULT_PATTERNS if p in requested]
    return ordered


def build_single_option_items(
    args: argparse.Namespace,
    timeout_sec: int,
    patterns: List[str],
    pattern_transports: Dict[str, List[str]],
    pattern_sizes: Dict[str, List[int]],
) -> List[Tuple[str, str]]:
    transports: List[str] = []
    sizes: List[int] = []
    for pattern in patterns:
        transports.extend(pattern_transports.get(pattern, []))
        sizes.extend(pattern_sizes.get(pattern, []))

    unique_transports = sorted(set(transports))
    unique_sizes = sorted(set(sizes))
    manual_socket_overrides = (
        (env_get("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES") or "")
        or (env_get("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES") or "")
    ) == "1"
    base_hwm = parse_env_int("PERF_SINGLE_HWM", 0) if manual_socket_overrides else 0
    sndhwm = parse_env_int("PERF_SINGLE_SNDHWM", base_hwm) if manual_socket_overrides else 0
    rcvhwm = parse_env_int("PERF_SINGLE_RCVHWM", base_hwm) if manual_socket_overrides else 0
    sndtimeo_ms = parse_env_int("PERF_SINGLE_SNDTIMEO_MS", 200)
    rcvtimeo_ms = parse_env_int("PERF_SINGLE_RCVTIMEO_MS", 200)
    io_threads = max(1, parse_env_int("PERF_IO_THREADS", 1))
    sndbuf = env_get("PERF_SINGLE_SNDBUF") if manual_socket_overrides else ""
    rcvbuf = env_get("PERF_SINGLE_RCVBUF") if manual_socket_overrides else ""
    items: List[Tuple[str, str]] = [
        ("runs", str(args.runs)),
        ("duration_seconds", str(parse_env_int("PERF_SINGLE_DURATION_SECONDS", 5))),
        ("timeout_seconds", str(timeout_sec)),
        ("fail_fast", "1" if FAIL_FAST else "0"),
        ("io_threads", str(io_threads)),
        ("hwm", "auto-hwm" if base_hwm <= 0 else str(base_hwm)),
        ("sndhwm", "auto-hwm" if sndhwm <= 0 else str(sndhwm)),
        ("rcvhwm", "auto-hwm" if rcvhwm <= 0 else str(rcvhwm)),
        ("sndbuf", sndbuf if sndbuf else "-1"),
        ("rcvbuf", rcvbuf if rcvbuf else "-1"),
        ("sndtimeo_ms", str(sndtimeo_ms)),
        ("rcvtimeo_ms", str(rcvtimeo_ms)),
        ("ctx_auto_hwm_enable", env_get("PERF_CTX_AUTO_HWM_ENABLE") or "core-default"),
        ("ctx_auto_hwm_profile", env_get("PERF_CTX_AUTO_HWM_PROFILE") or "balanced"),
        ("patterns", ",".join(patterns)),
        ("transports", ",".join(unique_transports) if unique_transports else "none"),
        ("msg_sizes", ",".join(str(sz) for sz in unique_sizes) if unique_sizes else "none"),
    ]
    return items


def print_effective_options(label: str, items: List[Tuple[str, str]]) -> None:
    print(f"\n## Effective Options ({label})")
    print(f"- lang: {RESULT_LANG}")
    print(f"- suite: {RESULT_SUITE}")
    for key, value in items:
        print(f"- {key}: {value}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure current zlink dotnet single-pattern benchmarks."
    )
    parser.add_argument("pattern", nargs="?", default="ALL")
    parser.add_argument("--runs", type=int, default=None)
    parser.add_argument("--duration", type=int, default=None)
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--pin-cpu", action="store_true")
    parser.add_argument("--results-dir", default=DEFAULT_RESULTS_DIR)
    parser.add_argument("--results-tag", default="")
    parser.add_argument("--result-file", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if not args.results_tag:
        args.results_tag = env_get("PERF_RESULTS_TAG").strip()

    if args.runs is None:
        args.runs = 1
    if args.runs < 1:
        print("Error: --runs must be >= 1.", file=sys.stderr)
        return 1
    if args.duration is not None and args.duration < 1:
        print("Error: --duration must be >= 1.", file=sys.stderr)
        return 1
    if args.duration is not None:
        os.environ["PERF_SINGLE_DURATION_SECONDS"] = str(args.duration)

    try:
        patterns = parse_pattern_arg(args.pattern)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    binary_path = resolve_dotnet_binary()
    if not os.path.isfile(binary_path):
        print(
            "Error: dotnet single benchmark binary not found: "
            f"{binary_path}",
            file=sys.stderr,
        )
        return 1

    default_timeout_sec = max(
        30,
        parse_env_int("PERF_SINGLE_DURATION_SECONDS", 5) * 6 + 15,
    )
    timeout_sec = max(
        1,
        parse_env_int("PERF_SINGLE_TIMEOUT_SECONDS", default_timeout_sec),
    )

    results_root = os.path.abspath(args.results_dir)
    result_dir = single_result_dir(results_root)
    result_file = args.result_file
    if not result_file:
        result_file = os.path.join(
            result_dir, build_result_filename(args.results_tag)
        )
    result_parent = os.path.dirname(result_file)
    if result_parent:
        os.makedirs(result_parent, exist_ok=True)
    result_log_fh = open(result_file, "w", encoding="utf-8", buffering=1)
    orig_stdout = sys.stdout
    orig_stderr = sys.stderr
    sys.stdout = TeeStream(orig_stdout, result_log_fh)
    sys.stderr = TeeStream(orig_stderr, result_log_fh)

    pattern_transports: Dict[str, List[str]] = {}
    pattern_sizes: Dict[str, List[int]] = {}
    requested_combo_count = 0
    for pattern in patterns:
        transports = select_transports(pattern)
        sizes = msg_sizes_for_pattern(pattern)
        pattern_transports[pattern] = transports
        pattern_sizes[pattern] = sizes
        requested_combo_count += len(transports) * len(sizes)

    effective_options = build_single_option_items(
        args,
        timeout_sec,
        patterns,
        pattern_transports,
        pattern_sizes,
    )
    for key, value in build_meta_items(args.runs):
        print(f"META,{key},{value}")
    print_effective_options("start", effective_options)

    all_failures: List[Tuple[str, str, int, str]] = []
    combo_results: Dict[Tuple[str, str, int], ComboRecord] = {}
    run_warnings: List[str] = []
    auto_hwm_detail_rows: List[Dict[str, str]] = []
    table_lines: List[str] = []
    transport_transition_ms = max(
        0,
        parse_env_int("PERF_TRANSPORT_TRANSITION_MS", 3000),
    )

    for pattern in patterns:
        transports = pattern_transports.get(pattern, [])
        sizes = pattern_sizes.get(pattern, [])

        if table_lines:
            table_lines.extend(["", PATTERN_SEPARATOR, ""])
            print("")
            print(PATTERN_SEPARATOR)
            print("")

        pattern_header = f"## PATTERN: {pattern} ({pattern_direction_label(pattern)})"
        print(pattern_header)
        table_lines.append(pattern_header)

        if not transports:
            line = f"  Skipping {pattern}: no matching transports."
            print(line)
            table_lines.append(line)
            continue
        if not sizes:
            line = f"  Skipping {pattern}: no message sizes configured."
            print(line)
            table_lines.append(line)
            continue

        bench_line = f"  > Benchmarking current for {pattern}..."
        print(bench_line)
        table_lines.append(bench_line)

        for transport_idx, transport in enumerate(transports):
            has_next_transport = (transport_idx + 1) < len(transports)
            testing_line = f"    Testing {transport}:"
            print(testing_line)
            table_lines.append(testing_line)

            show_run_labels = args.runs > 1
            if not show_run_labels:
                header = f"      {single_table_header_line()}"
                separator = f"      {single_table_separator_line()}"
                print(header)
                print(separator)
                table_lines.append(header)
                table_lines.append(separator)

            t_samples: Dict[int, List[float]] = {size: [] for size in sizes}
            b_samples: Dict[int, List[float]] = {size: [] for size in sizes}
            l_samples: Dict[int, List[float]] = {size: [] for size in sizes}
            l95_samples: Dict[int, List[float]] = {size: [] for size in sizes}
            l99_samples: Dict[int, List[float]] = {size: [] for size in sizes}
            failed_records: Dict[int, ComboRecord] = {}
            failed_sizes: Dict[int, str] = {}
            transport_unsupported = False
            transport_skip = False

            abort_pattern = False
            for run_idx in range(args.runs):
                if transport_unsupported or transport_skip:
                    break
                run_no = run_idx + 1
                if show_run_labels:
                    run_line = f"      run {run_no}/{args.runs}:"
                    print(run_line)
                    table_lines.append(run_line)
                    header = f"        {single_table_header_line()}"
                    separator = f"        {single_table_separator_line()}"
                    print(header)
                    print(separator)
                    table_lines.append(header)
                    table_lines.append(separator)
                    row_indent = "        "
                else:
                    row_indent = "      "

                for size_idx, size in enumerate(sizes):
                    outcome = run_single_test(
                        binary_path,
                        pattern,
                        transport,
                        size,
                        timeout_sec,
                        args.pin_cpu,
                    )

                    if outcome.warnings:
                        for warning in outcome.warnings:
                            run_warnings.append(
                                f"{pattern} {transport} {size}B run#{run_no}: {warning}"
                            )
                    if outcome.auto_hwm_details:
                        auto_hwm_detail_rows.extend(outcome.auto_hwm_details)

                    if outcome.status == "success":
                        t_samples[size].append(outcome.throughput)
                        b_samples[size].append(outcome.bandwidth)
                        l_samples[size].append(outcome.latency)
                        l95_samples[size].append(outcome.latency_p95)
                        l99_samples[size].append(outcome.latency_p99)
                        row = single_table_row_line(
                            size,
                            "success",
                            ComboRecord(
                                status="success",
                                throughput=outcome.throughput,
                                bandwidth=outcome.bandwidth,
                                latency=outcome.latency,
                                latency_p95=outcome.latency_p95,
                                latency_p99=outcome.latency_p99,
                            ),
                            pattern,
                        )
                        line = f"{row_indent}{row}"
                        print(line)
                        table_lines.append(line)
                        continue

                    if outcome.status == "unsupported":
                        transport_unsupported = True
                        for remain_size in sizes[size_idx:]:
                            row = single_table_row_line(
                                remain_size, "unsupported", None, pattern
                            )
                            line = f"{row_indent}{row}"
                            print(line)
                            table_lines.append(line)
                        break

                    if outcome.status == "skip":
                        transport_skip = True
                        reason = outcome.reason or "skip"
                        skip_record = ComboRecord(status="fail")
                        for remain_size in sizes[size_idx:]:
                            failed_sizes[remain_size] = reason
                            row_record = skip_record if remain_size == size else None
                            if row_record is not None:
                                failed_records[remain_size] = row_record
                            row = single_table_row_line(
                                remain_size, "fail", row_record, pattern
                            )
                            line = f"{row_indent}{row}"
                            print(line)
                            table_lines.append(line)
                        break

                    reason = outcome.reason or "fail"
                    failed_sizes[size] = reason
                    all_failures.append((pattern, transport, size, reason))
                    failed_record = ComboRecord(status="fail")
                    failed_records[size] = failed_record
                    row = single_table_row_line(size, "fail", failed_record, pattern)
                    line = f"{row_indent}{row}"
                    print(line)
                    table_lines.append(line)
                    if FAIL_FAST:
                        abort_pattern = True
                        break

                if abort_pattern:
                    break

            if transport_unsupported:
                for size in sizes:
                    combo_results[(pattern, transport, size)] = ComboRecord(status="unsupported")
            elif transport_skip:
                for size in sizes:
                    combo_results[(pattern, transport, size)] = ComboRecord(status="skip")
            else:
                for size in sizes:
                    if (
                        size in failed_sizes
                        or not t_samples[size]
                        or not b_samples[size]
                        or not l_samples[size]
                        or not l95_samples[size]
                        or not l99_samples[size]
                    ):
                        combo_results[(pattern, transport, size)] = failed_records.get(
                            size, ComboRecord(status="fail")
                        )
                        if size not in failed_sizes:
                            all_failures.append((pattern, transport, size, "no_data"))
                        continue

                    throughput = statistics.median(t_samples[size])
                    bandwidth = statistics.median(b_samples[size])
                    latency = statistics.median(l_samples[size])
                    latency_p95 = statistics.median(l95_samples[size])
                    latency_p99 = statistics.median(l99_samples[size])

                    combo_results[(pattern, transport, size)] = ComboRecord(
                        status="success",
                        throughput=throughput,
                        bandwidth=bandwidth,
                        latency=latency,
                        latency_p95=latency_p95,
                        latency_p99=latency_p99,
                    )

            if show_run_labels:
                median_line = "      median:"
                print(median_line)
                table_lines.append(median_line)
                header = f"        {single_table_header_line()}"
                separator = f"        {single_table_separator_line()}"
                print(header)
                print(separator)
                table_lines.append(header)
                table_lines.append(separator)

                for size in sizes:
                    record = combo_results.get((pattern, transport, size))
                    if record and record.status == "success":
                        row = single_table_row_line(size, "success", record, pattern)
                    elif record and record.status == "unsupported":
                        row = single_table_row_line(size, "unsupported", None, pattern)
                    else:
                        row = single_table_row_line(size, "fail", record, pattern)
                    line = f"        {row}"
                    print(line)
                    table_lines.append(line)

            done_line = f"    Testing {transport}: Done"
            print(done_line)
            table_lines.append(done_line)
            if transport_transition_ms > 0 and has_next_transport:
                cooldown_ms = transport_transition_ms
                next_transport = transports[transport_idx + 1]
                if next_transport == "inproc" and transport in ("ws", "wss"):
                    cooldown_ms = max(cooldown_ms, 30000)
                cooldown_line = f"    [transport cooldown {cooldown_ms}ms]"
                print(cooldown_line)
                table_lines.append(cooldown_line)
                time.sleep(cooldown_ms / 1000.0)

            if FAIL_FAST and all_failures:
                break

        if FAIL_FAST and all_failures:
            break

    if all_failures:
        print("\n## Failures")
        for pattern, transport, size, reason in all_failures:
            print(f"- {pattern} current {transport} {size}B: {reason}")

    for pattern in patterns:
        emit_auto_hwm_detail_table(auto_hwm_detail_rows, pattern)

    unsupported_combo_count = sum(
        1 for record in combo_results.values() if record.status == "unsupported"
    )
    skip_combo_count = sum(1 for record in combo_results.values() if record.status == "skip")
    success_combo_count = sum(
        1 for record in combo_results.values() if record.status == "success"
    )
    fail_combo_count = sum(1 for record in combo_results.values() if record.status == "fail")
    expected_result_lines = max(
        0,
        (requested_combo_count - unsupported_combo_count - skip_combo_count)
        * REQUIRED_RESULT_METRIC_COUNT,
    )
    actual_result_lines = sum(
        REQUIRED_RESULT_METRIC_COUNT
        for record in combo_results.values()
        if record.status == "success"
    )
    completion_status = (
        "complete" if expected_result_lines == actual_result_lines else "partial"
    )

    print_effective_options("result", effective_options)
    if any(record.status == "success" for record in combo_results.values()):
        print("\n## Result Data")
        emit_result_lines(combo_results)
    print("\n## Completion")
    print(f"- success: {success_combo_count}")
    print(f"- unsupported: {unsupported_combo_count}")
    print(f"- skip: {skip_combo_count}")
    print(f"- fail: {fail_combo_count}")
    print(f"- status: {completion_status}")
    print(f"- expected_result_lines: {expected_result_lines}")
    print(f"- actual_result_lines: {actual_result_lines}")

    if run_warnings:
        print("\n## Warnings")
        for item in run_warnings:
            print(f"- {item}")

    enforce_file_retention(os.path.dirname(result_file))
    print(f"\nSaved result file: {result_file} (status={completion_status})")

    sys.stdout = orig_stdout
    sys.stderr = orig_stderr
    result_log_fh.close()
    if completion_status != "complete":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
