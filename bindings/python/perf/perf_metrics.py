import importlib
import math
import os
import platform
import socket
import struct
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime
from pathlib import Path


DEFAULT_READY_TIMEOUT_MS = 1000
HEADER_MAGIC = 0x5A4C4E4B
HEADER_FORMAT = "<IIBIQq"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
HEADER_MAGIC_BYTES = struct.pack("<I", HEADER_MAGIC)
_zlink = None
_seq = 0
_tls_assets = None


def _env_int(name, default):
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    try:
        return int(value)
    except ValueError:
        return default


def env_int(name, default):
    return _env_int(name, default)


def benchmark_run_id():
    run_id = _env_int("PERF_RUN_ID", 1)
    if run_id <= 0:
        return 1
    return run_id & 0xFFFFFFFF


def _next_seq():
    global _seq
    seq = _seq
    _seq += 1
    return seq


def decode_header(data):
    if len(data) < HEADER_SIZE:
        return None
    magic, run_id, phase, msg_size, seq, sent_ts_ns = struct.unpack_from(
        HEADER_FORMAT, data, 0
    )
    return {
        "magic": magic,
        "run_id": run_id,
        "phase": phase,
        "msg_size": msg_size,
        "seq": seq,
        "sent_ts_ns": sent_ts_ns,
    }


def latency_ns_from_message(data):
    header = decode_header(data)
    if header is None or header["magic"] != HEADER_MAGIC:
        raise RuntimeError("invalid perf message header")
    now_ns = time.time_ns()
    if header["sent_ts_ns"] <= 0 or now_ns < header["sent_ts_ns"]:
        return None
    return float(now_ns - header["sent_ts_ns"])


def extract_metric_payload(parts, *, expected_size=None):
    # C perf_single_one_way.hpp recv_single_part_header_flags: the metric
    # payload is the final data part decoded strictly at offset 0; the
    # header magic must sit at byte 0 and (when known) the part size must
    # equal the expected payload size exactly. No arbitrary-offset scan.
    if not parts:
        return b""
    part = parts[-1]
    if part is None:
        return b""
    data = bytes(part)
    if expected_size is not None and len(data) != expected_size:
        return b""
    header = decode_header(data)
    if header is not None and header["magic"] == HEADER_MAGIC:
        return data
    return b""


def is_active_message(data, *, expected_msg_size=None, run_id=None):
    header = decode_header(data)
    if header is None:
        return False
    if header["magic"] != HEADER_MAGIC:
        return False
    if header["phase"] != 1:
        return False
    if expected_msg_size is not None and header["msg_size"] != expected_msg_size:
        return False
    if run_id is not None and header["run_id"] != run_id:
        return False
    return True


def payload_phase(data):
    header = decode_header(data)
    if header is None:
        return 0
    return header["phase"]


def stamp_payload(payload, phase=0, *, run_id=None, seq=None):
    header_run_id = benchmark_run_id() if run_id is None else (run_id & 0xFFFFFFFF)
    header_seq = _next_seq() if seq is None else seq
    struct.pack_into(
        HEADER_FORMAT,
        payload,
        0,
        HEADER_MAGIC,
        header_run_id,
        int(phase),
        len(payload),
        int(header_seq),
        int(time.time_ns()),
    )
    return payload


def new_payload(size):
    return bytearray(size)


def percentile(values, ratio):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * ratio) - 1))
    return float(ordered[index])


def result_metrics(
    *,
    count,
    msg_size,
    elapsed_s,
    latencies_ns,
    bandwidth_multiplier=1.0,
):
    throughput = (count / elapsed_s) if elapsed_s > 0 else 0.0
    bandwidth = (
        (count * msg_size * bandwidth_multiplier) / elapsed_s / 1_000_000.0
        if elapsed_s > 0
        else 0.0
    )
    mean = (sum(latencies_ns) / len(latencies_ns)) if latencies_ns else 0.0
    return {
        "throughput": throughput,
        "bandwidth": bandwidth,
        "latency": float(mean) / 1_000_000.0,
        "latency_p95": percentile(latencies_ns, 0.95) / 1_000_000.0,
        "latency_p99": percentile(latencies_ns, 0.99) / 1_000_000.0,
    }


def print_result_lines(pattern, transport, msg_size, metrics):
    for name in ("throughput", "bandwidth", "latency", "latency_p95", "latency_p99"):
        value = f"{metrics[name]:.3f}"
        print(f"RESULT,current,{pattern},{transport},{msg_size},{name},{value}", flush=True)


def unique_endpoint(prefix):
    return f"inproc://py-perf-{prefix}-{os.getpid()}-{uuid.uuid4().hex}"


def _reserve_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def tcp_endpoint(prefix="perf"):
    _ = prefix
    return f"tcp://127.0.0.1:{_reserve_tcp_port()}"


def transport_endpoint(transport, prefix="perf"):
    transport = transport.lower()
    if transport in {"tcp", "tls", "ws", "wss"}:
        return f"{transport}://127.0.0.1:{_reserve_tcp_port()}"
    if transport == "inproc":
        return unique_endpoint(prefix)
    if transport == "ipc":
        sock_path = Path(tempfile.gettempdir()) / (
            f"zlink-py-perf-{prefix}-{os.getpid()}-{uuid.uuid4().hex}.sock"
        )
        return f"ipc://{sock_path}"
    raise ValueError(f"unsupported transport: {transport}")


def is_tls_transport(transport):
    return str(transport).lower() in {"tls", "wss"}


def _resolve_tls_assets():
    global _tls_assets
    if _tls_assets is not None:
        return _tls_assets
    current = Path.cwd().resolve()
    while True:
        candidate = current / "core" / "tests" / "certs" / "gen"
        ca = candidate / "ca.crt"
        cert = candidate / "server.crt"
        key = candidate / "server.key"
        if ca.exists() and cert.exists() and key.exists():
            _tls_assets = {
                "ca": str(ca),
                "cert": str(cert),
                "key": str(key),
            }
            return _tls_assets
        if current.parent == current:
            break
        current = current.parent
    raise RuntimeError("TLS perf assets not found under core/tests/certs/gen")


def configure_tls_server(target, transport):
    if not is_tls_transport(transport):
        return
    assets = _resolve_tls_assets()
    target.set_tls_server(assets["cert"], assets["key"], False)


def configure_tls_client(target, transport, *, hostname="localhost", trust_system=False):
    if not is_tls_transport(transport):
        return
    assets = _resolve_tls_assets()
    target.set_tls_client(assets["ca"], hostname, trust_system)


def resolve_single_timeout_seconds(duration_seconds):
    override = _env_int("PERF_SINGLE_TIMEOUT_SECONDS", 0)
    if override > 0:
        return override
    return max(30, int(math.ceil(float(duration_seconds) * 6.0)) + 15)


def wait_monitor_event(monitor, event_mask, *, timeout_ms=DEFAULT_READY_TIMEOUT_MS):
    result = {}
    failure = {}
    done = threading.Event()

    def _recv_event():
        try:
            result["event"] = monitor.recv()
        except Exception as exc:  # pragma: no cover - propagated below
            failure["exc"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=_recv_event, daemon=True)
    thread.start()
    if not done.wait(timeout_ms / 1000.0):
        raise RuntimeError(
            f"timed out waiting for socket monitor event {int(event_mask)}"
        )
    if "exc" in failure:
        raise failure["exc"]
    event = result["event"]
    if not (int(event.event) & int(event_mask)):
        raise RuntimeError(
            f"unexpected socket monitor event {int(event.event)} while waiting for {int(event_mask)}"
        )
    return event


def resolve_single_connect_ready_timeout_ms():
    return _env_int("PERF_CONNECT_READY_TIMEOUT_MS", DEFAULT_READY_TIMEOUT_MS)


def resolve_multi_connect_ready_timeout_ms():
    return _env_int(
        "PERF_MULTI_CONNECT_READY_TIMEOUT_MS",
        _env_int("PERF_CONNECT_READY_TIMEOUT_MS", DEFAULT_READY_TIMEOUT_MS),
    )


def _require_zlink():
    global _zlink
    if _zlink is None:
        _zlink = importlib.import_module("zlink")
    return _zlink


def safe_poll(poller, events, timeout_ms):
    zlink_mod = _require_zlink()
    try:
        return poller.wait(events, timeout_ms)
    except zlink_mod.ZlinkError as exc:
        if exc.native_errno == 11:
            return 0
        raise


def pin_current_process_cpu0():
    if platform.system().lower() != "linux":
        return False
    if not hasattr(os, "sched_setaffinity"):
        return False
    try:
        os.sched_setaffinity(0, {0})
    except OSError:
        return False
    return True


# ---------------------------------------------------------------------------
# Reporting helpers (shared by single & multi runners)
# ---------------------------------------------------------------------------


def platform_name():
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith(("win32", "cygwin", "msys")):
        return "windows"
    return sys.platform


def _retain_latest_report_files(report_dir, max_files):
    if max_files <= 0 or not report_dir.exists():
        return
    files = sorted(path for path in report_dir.iterdir() if path.is_file())
    while len(files) > max_files:
        oldest = files.pop(0)
        try:
            oldest.unlink()
        except OSError:
            break


def build_report_path(*, lang, suite, results_dir=None, tag=None):
    report_root = (
        Path(__file__).resolve().parent / "results"
        if results_dir is None
        else Path(results_dir)
    )
    report_dir = report_root / suite / "report"
    report_dir.mkdir(parents=True, exist_ok=True)
    max_files = (
        _env_int("PERF_RESULTS_MAX_FILES", 100)
        if suite == "multi"
        else 100
    )
    _retain_latest_report_files(report_dir, max_files)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    suffix = f"_{tag}" if tag else ""
    return report_dir / f"perf_{lang}_{suite}_{platform_name()}_{timestamp}{suffix}.txt"


def render_effective_options(options, *, section="start"):
    lines = [f"## Effective Options ({section})"]
    for key, value in options.items():
        lines.append(f"- {key}: {value}")
    return "\n".join(lines)


def _default_warn(message):
    print(f"warning: {message}", file=sys.stderr, flush=True)


def parse_result_lines(output, *, warn=None):
    rows = []
    warn_fn = _default_warn if warn is True else warn
    for line in output.splitlines():
        if not line.startswith("RESULT,"):
            continue
        parts = line.split(",")
        if len(parts) != 7:
            if warn_fn is not None:
                warn_fn(f"ignored malformed RESULT line: {line}")
            continue
        _, lib, pattern, transport, size, metric, value = parts
        rows.append(
            {
                "lib": lib,
                "pattern": pattern,
                "transport": transport,
                "size": size,
                "metric": metric,
                "value": value,
            }
        )
    return rows


def rows_by_case(rows, *, warn=None):
    grouped = {}
    warn_fn = _default_warn if warn is True else warn
    for row in rows:
        key = (row["pattern"], row["transport"], str(row["size"]))
        if warn_fn is not None and key in grouped and row["metric"] in grouped[key]:
            warn_fn(
                "duplicate RESULT metric overwritten for "
                f"{row['pattern']} {row['transport']} {row['size']} {row['metric']}"
            )
        grouped.setdefault(key, {})[row["metric"]] = row["value"]
    return grouped


def pattern_direction_label(pattern):
    if pattern in {
        "MULTI_DEALER_ROUTER",
        "MULTI_ROUTER_ROUTER",
        "MULTI_STREAM",
    }:
        return "echo"
    return "one-way"


def throughput_unit(pattern):
    return "Kops/s" if pattern_direction_label(pattern) == "echo" else "Kmsg/s"


def _metric_row_text(pattern, size, metrics):
    throughput = f"{float(metrics.get('throughput', 0.0)) / 1000.0:8.3f} {throughput_unit(pattern)}"
    return (
        f"| {str(size) + 'B':<8} | "
        f"{throughput:>16} | "
        f"{float(metrics.get('bandwidth', 0.0)):>10.3f} MB/s | "
        f"{float(metrics.get('latency', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p95', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p99', 0.0)):>9.3f} ms |"
    )


def table_header_lines():
    return [
        "| Size     |         Throughput |      Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |",
        "|----------|--------------------|----------------|---------------|---------------|---------------|",
    ]


def status_row_text(size, status):
    return (
        f"| {str(size) + 'B':<8} | "
        f"{status:>16} | "
        f"{status:>12} | "
        f"{status:>13} | "
        f"{status:>13} | "
        f"{status:>13} |"
    )


def render_result_tables(rows):
    if not rows:
        return ""
    grouped = rows_by_case(rows)
    lines = []
    last_pattern = None
    last_transport = None
    for pattern, transport, size in sorted(grouped):
        if pattern != last_pattern:
            if last_pattern is not None:
                lines.extend(["", "===============================================================================", ""])
            lines.append(f"## PATTERN: {pattern} ({pattern_direction_label(pattern)})")
            lines.append("")
            last_pattern = pattern
            last_transport = None
        if transport != last_transport:
            if last_transport is not None:
                lines.append("")
            lines.append(f"### Transport: {transport}")
            lines.extend(table_header_lines())
            last_transport = transport
        lines.append(_metric_row_text(pattern, size, grouped[(pattern, transport, size)]))
    return "\n".join(lines)


def render_markdown_summary(rows):
    return render_result_tables(rows)
