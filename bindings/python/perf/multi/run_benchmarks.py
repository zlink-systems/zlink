import argparse
import os
import platform
import queue
import signal
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path
from datetime import datetime

from perf_multi_common import (
    build_report_path,
    parse_result_lines,
    pin_current_process_cpu0,
    PYTHON_MULTI_DEFAULT_IO_THREADS,
    render_effective_options,
    resolve_multi_timeout_seconds,
    rows_by_case,
    status_row_text,
    table_header_lines,
    throughput_unit,
)
from perf_report import multi_auto_hwm_lines, sort_result_data_lines
from perf_runtime import configure_runtime


ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parent.parent.parent.parent
DEFAULT_PYTHONPATH = ROOT.parent.parent / "src"
# MULTI_STREAM client must be the shared C perf_stream_client binary so the
# measured surface stays the Python STREAM server (see PERF_MULTI_TEST_POLICY).
STREAM_CLIENT_DIR = REPO_ROOT / "bindings" / "c" / "perf" / "common" / "streamclient"
STREAM_CLIENT_BIN = STREAM_CLIENT_DIR / "build" / "perf_stream_client"


def _ensure_stream_client():
    if STREAM_CLIENT_BIN.exists() and os.access(STREAM_CLIENT_BIN, os.X_OK):
        return STREAM_CLIENT_BIN
    subprocess.run(
        ["bash", str(STREAM_CLIENT_DIR / "build.sh")],
        check=True,
        capture_output=True,
        text=True,
    )
    return STREAM_CLIENT_BIN
DEFAULT_PATTERNS = (
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "ROUTER_ROUTER",
    "PUBSUB",
    "STREAM",
)
DEFAULT_MSG_SIZES = ("64", "256", "1024", "4096", "65536", "131072")
DEFAULT_STREAM_MSG_SIZES = ("64", "256", "1024", "65536")
DEALER_DEALER_SERVER_SHUTDOWN_TIMEOUT_MS = "30000"
RAW_TRANSPORTS = ("tcp", "tls", "ws", "wss")
POLICY_TRANSPORTS = {
    "DEALER_DEALER": RAW_TRANSPORTS,
    "DEALER_ROUTER": RAW_TRANSPORTS,
    "ROUTER_ROUTER": RAW_TRANSPORTS,
    "PUBSUB": RAW_TRANSPORTS,
    "STREAM": ("tcp", "tls", "ws", "wss"),
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
    parser = argparse.ArgumentParser(prog="run_benchmarks_multi.sh")
    parser.add_argument("--pattern", default="ALL")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--reuse-build", action="store_true")
    parser.add_argument("--clean-build", action="store_true")
    parser.add_argument(
        "--duration",
        default=os.environ.get("PERF_MULTI_DURATION_SECONDS", "5"),
    )
    parser.add_argument("--msg-sizes", default="")
    parser.add_argument("--transports", default="")
    parser.add_argument("--runs", default="1")
    parser.add_argument("--clients", default=None)
    parser.add_argument("--results-dir", default="")
    parser.add_argument("--results-tag", default="")
    parser.add_argument("--output", default="")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--pin-cpu", action="store_true")
    parser.add_argument("--io-threads", default="")
    parser.add_argument("--server-io-threads", default="")
    parser.add_argument("--client-io-threads", default="")
    parser.add_argument("--hwm", default="")
    parser.add_argument("--send-hwm", default="")
    parser.add_argument("--recv-hwm", default="")
    parser.add_argument("--buf", default="")
    parser.add_argument("--sndbuf", default="")
    parser.add_argument("--rcvbuf", default="")
    parser.add_argument("--sndtimeo", "--send-timeout-ms", dest="send_timeout_ms", default="")
    parser.add_argument("--rcvtimeo", "--recv-timeout-ms", dest="recv_timeout_ms", default="")
    parser.add_argument("--connect-concurrency", default="")
    parser.add_argument("--transport-transition-ms", default="")
    parser.add_argument("--pattern-transition-ms", default="")
    parser.add_argument("--server-ready-timeout-ms", default="")
    parser.add_argument("--connect-ready-timeout-ms", default="")
    parser.add_argument("--monitor-hwm", default="")
    parser.add_argument("--server-shutdown-timeout-ms", default="")
    parser.add_argument("--server-bind-port", default="")
    parser.add_argument("--auto-hwm-profile", default="")
    return parser.parse_args(argv)


def _parse_csv(value):
    return [item.strip() for item in value.split(",") if item.strip()]


def _normalize_pattern(value):
    pattern = value.strip().upper()
    if pattern.startswith("MULTI_"):
        pattern = pattern[6:]
    if pattern == "STREAMS":
        pattern = "STREAM"
    return pattern


def _parse_patterns(value):
    text = value.strip().upper()
    if text == "ALL":
        return list(DEFAULT_PATTERNS)
    patterns = [_normalize_pattern(item) for item in value.split(",") if item.strip()]
    if not patterns:
        raise SystemExit("unsupported pattern: ")
    unknown = [pattern for pattern in patterns if pattern not in DEFAULT_PATTERNS]
    if unknown:
        raise SystemExit(f"unsupported pattern: {unknown[0]}")
    return patterns


def _requested_msg_sizes(args):
    if args.msg_sizes:
        sizes = _parse_csv(args.msg_sizes)
        if not sizes:
            raise SystemExit("--msg-sizes must not be empty")
        return sizes
    return None


def _msg_sizes_for_pattern(pattern, requested_msg_sizes):
    if requested_msg_sizes is not None:
        return requested_msg_sizes
    if pattern == "STREAM":
        stream_env = os.environ.get("PERF_MULTI_STREAM_MSG_SIZES", "") or os.environ.get(
            "PERF_STREAM_MSG_SIZES", ""
        )
        if stream_env:
            sizes = _parse_csv(stream_env)
            if not sizes:
                raise SystemExit("PERF_MULTI_STREAM_MSG_SIZES/PERF_STREAM_MSG_SIZES must not be empty")
            return sizes
    common_env = os.environ.get("PERF_MSG_SIZES", "")
    if common_env:
        sizes = _parse_csv(common_env)
        if not sizes:
            raise SystemExit("PERF_MSG_SIZES must not be empty")
        return sizes
    if pattern == "STREAM":
        return list(DEFAULT_STREAM_MSG_SIZES)
    return list(DEFAULT_MSG_SIZES)


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


def _grouped_option_text(patterns, value_for_pattern, *, prefix="MULTI_"):
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
        rendered.append(
            f"{','.join(f'{prefix}{pattern}' for pattern in grouped_patterns)}={','.join(values)}"
        )
    return "; ".join(rendered)


def _needs_dealer_dealer_shutdown_timeout_default(configs):
    return any(pattern == "DEALER_DEALER" for pattern, _transport, _msg_size in configs)


def _apply_dealer_dealer_shutdown_timeout_default(args, env, configs):
    if not _needs_dealer_dealer_shutdown_timeout_default(configs):
        return
    if (
        not args.server_shutdown_timeout_ms
        and "PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS" not in env
        and "PERF_SERVER_SHUTDOWN_TIMEOUT_MS" not in env
    ):
        env["PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS"] = (
            DEALER_DEALER_SERVER_SHUTDOWN_TIMEOUT_MS
        )


def _effective_role_io_threads(args, role):
    role_arg = args.server_io_threads if role == "server" else args.client_io_threads
    role_env = (
        "PERF_MULTI_SERVER_IO_THREADS"
        if role == "server"
        else "PERF_MULTI_CLIENT_IO_THREADS"
    )
    if role_arg:
        return role_arg
    if args.io_threads:
        return args.io_threads
    if os.environ.get(role_env):
        return os.environ[role_env]
    if os.environ.get("PERF_IO_THREADS"):
        return f"{os.environ['PERF_IO_THREADS']} (from PERF_IO_THREADS)"
    default_io_threads = os.environ.get("PERF_MULTI_DEFAULT_IO_THREADS") or os.environ.get(
        "PERF_DEFAULT_IO_THREADS"
    )
    if default_io_threads:
        return f"{default_io_threads} (python default)"
    return f"{PYTHON_MULTI_DEFAULT_IO_THREADS} (python default)"


def _selected_configs(patterns, transports, requested_msg_sizes):
    configs = []
    for pattern in patterns:
        for transport in _transports_for_pattern(pattern, transports):
            for msg_size in _msg_sizes_for_pattern(pattern, requested_msg_sizes):
                configs.append((pattern, transport, msg_size))
    if not configs:
        raise SystemExit("no runnable pattern/transport combinations selected")
    return configs


def _default_clients(patterns):
    if patterns and all(pattern == "STREAM" for pattern in patterns):
        return os.environ.get("PERF_MULTI_DEFAULT_STREAM_CLIENTS") or os.environ.get(
            "PERF_STREAM_DEFAULT_CLIENTS", "10000"
        )
    return "policy-default"


def _options_clients_display(patterns, cli_value):
    if cli_value is not None:
        return cli_value
    env_value = os.environ.get("PERF_MULTI_CLIENTS") or os.environ.get("PERF_CLIENTS")
    if env_value:
        return env_value
    default_clients = os.environ.get("PERF_MULTI_DEFAULT_CLIENTS") or os.environ.get(
        "PERF_DEFAULT_CLIENTS", "100"
    )
    default_stream_clients = os.environ.get("PERF_MULTI_DEFAULT_STREAM_CLIENTS") or os.environ.get(
        "PERF_STREAM_DEFAULT_CLIENTS", "10000"
    )
    if patterns and all(pattern == "STREAM" for pattern in patterns):
        return default_stream_clients
    if any(pattern == "STREAM" for pattern in patterns):
        return f"{default_clients} (stream={default_stream_clients})"
    return default_clients


def _clients_for_pattern(pattern, cli_value):
    if cli_value is not None:
        return cli_value
    env_value = os.environ.get("PERF_MULTI_CLIENTS") or os.environ.get("PERF_CLIENTS")
    if env_value:
        return env_value
    if pattern == "STREAM":
        return os.environ.get("PERF_MULTI_DEFAULT_STREAM_CLIENTS") or os.environ.get(
            "PERF_STREAM_DEFAULT_CLIENTS", "10000"
        )
    return os.environ.get("PERF_MULTI_DEFAULT_CLIENTS") or os.environ.get(
        "PERF_DEFAULT_CLIENTS", "100"
    )


def _connect_concurrency_for_clients(clients):
    env_value = os.environ.get("PERF_MULTI_CONNECT_CONCURRENCY") or os.environ.get("PERF_CONNECT_CONCURRENCY")
    if env_value:
        return env_value
    parsed = _uint(clients)
    return "1024" if parsed is not None and parsed >= 10000 else "128"


def _uint(value):
    try:
        parsed = int(str(value))
    except (TypeError, ValueError):
        return None
    return parsed if parsed >= 0 else None


def _memory_available_kb():
    if os.environ.get("PERF_SKIP_MEMORY_CHECK") == "1":
        return None
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("MemAvailable:"):
                    parts = line.split()
                    return _uint(parts[1]) if len(parts) >= 2 else None
    except OSError:
        return None
    return None


def _memory_max_clients():
    available_kb = _memory_available_kb()
    if available_kb is None:
        return None
    budget_pct = _uint(os.environ.get("PERF_MULTI_MEMORY_BUDGET_PCT", os.environ.get("PERF_MEMORY_BUDGET_PCT", "70")))
    base_mb = _uint(os.environ.get("PERF_MULTI_MEMORY_BASE_MB", os.environ.get("PERF_MEMORY_BASE_MB", "512")))
    per_client_kb = _uint(os.environ.get("PERF_MULTI_MEMORY_PER_CLIENT_KB", os.environ.get("PERF_MEMORY_PER_CLIENT_KB", "1024")))
    if budget_pct is None or budget_pct < 1 or budget_pct > 95 or base_mb is None or per_client_kb is None or per_client_kb < 1:
        return None
    usable_kb = available_kb * budget_pct // 100
    base_kb = base_mb * 1024
    if usable_kb <= base_kb:
        return 1
    return max(1, (usable_kb - base_kb) // per_client_kb)


def _cap_default_clients_for_memory(clients, explicit_clients):
    if explicit_clients:
        return clients
    parsed = _uint(clients)
    max_clients = _memory_max_clients()
    if parsed is None or max_clients is None:
        return clients
    return str(min(parsed, max_clients))


def _resource_guard_skip(pattern, transport, msg_size, clients):
    parsed = _uint(clients)
    if parsed is None:
        return None
    if os.environ.get("PERF_SKIP_NOFILE_CHECK") != "1":
        try:
            import resource

            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            required = parsed * 3 + 4096
            if soft != resource.RLIM_INFINITY and soft < required:
                target = required if hard == resource.RLIM_INFINITY else min(required, hard)
                if target > soft:
                    try:
                        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
                        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
                    except (OSError, ValueError):
                        pass
            if soft != resource.RLIM_INFINITY and soft < required:
                return f"SKIP,current,MULTI_{pattern},{transport},nofile_guard:clients={clients},required={required},soft={soft},hard={hard}"
        except (ImportError, OSError, ValueError):
            pass
    if os.environ.get("PERF_SKIP_MEMORY_CHECK") != "1":
        max_clients = _memory_max_clients()
        if max_clients is not None and parsed > max_clients:
            return f"SKIP,current,MULTI_{pattern},{transport},memory_guard:clients={clients},max_clients={max_clients}"
    return None


def _server_popen_kwargs():
    kwargs = {}
    if os.name == "posix":
        kwargs["start_new_session"] = True
    return kwargs


def _coerce_text(chunk):
    if chunk is None:
        return ""
    if isinstance(chunk, bytes):
        return chunk.decode("utf-8", errors="replace")
    return str(chunk)


def _parse_status_lines(output):
    rows = []
    for line in output.splitlines():
        if line.startswith("SKIP,") or line.startswith("UNSUPPORTED,"):
            rows.append(line.strip())
    return rows


def _env_int(name, default, env_map=None):
    source = os.environ if env_map is None else env_map
    value = source.get(name)
    if value in (None, ""):
        return default
    try:
        return int(value)
    except ValueError:
        return default


def _arg_or_env_int(cli_value, env_name, default, env_map=None):
    if cli_value not in (None, ""):
        try:
            return int(cli_value)
        except ValueError:
            raise SystemExit(f"{env_name} must be an integer")
    return _env_int(env_name, default, env_map)


def _arg_or_env_pair_int(cli_value, primary_env, fallback_env, default, env_map=None):
    if cli_value not in (None, ""):
        try:
            return int(cli_value)
        except ValueError:
            raise SystemExit(f"{primary_env} must be an integer")
    return _env_int(primary_env, _env_int(fallback_env, default, env_map), env_map)


def _env_pair_value(primary_env, fallback_env, default, env_map=None):
    source = os.environ if env_map is None else env_map
    return source.get(primary_env) or source.get(fallback_env, default)


def _stream_completion_wait_ms():
    return os.environ.get("PERF_MULTI_STREAM_COMPLETION_WAIT_MS") or os.environ.get(
        "PERF_STREAM_COMPLETION_WAIT_MS", "10000"
    )


def _append_line(lines, line=""):
    print(line, flush=True)
    lines.append(line)


def _failure_reason(output):
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if not lines:
        return "no_data"
    return lines[-1]


def _result_metrics_for_case(output, pattern, transport, msg_size):
    grouped = rows_by_case(parse_result_lines(output, warn=_warn_runner), warn=_warn_runner)
    return grouped.get((f"MULTI_{pattern}", transport, str(msg_size)), {})


def _metric_row(pattern, msg_size, metrics, *, indent="      "):
    unit = throughput_unit(f"MULTI_{pattern}")
    throughput = f"{float(metrics.get('throughput', 0.0)) / 1000.0:8.3f} {unit}"
    return (
        f"{indent}| {str(msg_size) + 'B':<8} | "
        f"{throughput:>16} | "
        f"{float(metrics.get('bandwidth', 0.0)):>10.3f} MB/s | "
        f"{float(metrics.get('latency', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p95', 0.0)):>9.3f} ms | "
        f"{float(metrics.get('latency_p99', 0.0)):>9.3f} ms |"
    )


def pattern_direction(pattern):
    return "echo" if pattern in {
        "MULTI_DEALER_ROUTER",
        "MULTI_ROUTER_ROUTER",
        "MULTI_STREAM",
    } else "one-way"


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


def _is_unsupported_output(text):
    lowered = text.lower()
    markers = (
        "protocol not supported",
        "unsupported,current,",
    )
    return any(marker in lowered for marker in markers)


def _warn_runner(message):
    print(f"warning: {message}", file=sys.stderr, flush=True)


def _status_kind(output):
    if parse_result_lines(output):
        return "ok"
    for line in _parse_status_lines(output):
        if line.startswith("UNSUPPORTED,"):
            return "unsupported"
        if line.startswith("SKIP,"):
            return "skip"
    return "fail"


def _stdout_queue(proc):
    existing = getattr(proc, "_zlink_stdout_queue", None)
    if existing is not None:
        return existing
    lines = queue.Queue()

    def read_stdout():
        try:
            for line in proc.stdout:
                lines.put(line)
        finally:
            lines.put(None)

    threading.Thread(target=read_stdout, daemon=True).start()
    proc._zlink_stdout_queue = lines
    return lines


def _wait_for_control_line(proc, prefixes, *, timeout_s, stdout_chunks):
    deadline = time.perf_counter() + timeout_s
    prefix_tuple = tuple(prefixes)
    lines = _stdout_queue(proc)
    while time.perf_counter() < deadline:
        remaining = max(0.0, deadline - time.perf_counter())
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            break
        if not line:
            break
        text = line.strip()
        if not text:
            continue
        stdout_chunks.append(text)
        if text.startswith(prefix_tuple):
            return text
    wanted = ", ".join(prefixes)
    details = []
    if proc.poll() is not None:
        details.append(f"process exited with code {proc.returncode}")
        if proc.stderr:
            stderr_text = proc.stderr.read().strip()
            if stderr_text:
                details.append(stderr_text)
    detail_text = ": " + "\n".join(details) if details else ""
    raise SystemExit(f"missing control line: {wanted}{detail_text}")


def _drain_stdout_queue(proc, stdout_chunks):
    lines = getattr(proc, "_zlink_stdout_queue", None)
    if lines is None:
        return
    while True:
        try:
            line = lines.get_nowait()
        except queue.Empty:
            return
        if not line:
            return
        text = line.strip()
        if text:
            stdout_chunks.append(text)


def _terminate_process(proc, *, grace_seconds):
    if proc.poll() is not None:
        return
    try:
        if proc.stdin:
            proc.stdin.write("STOP\n")
            proc.stdin.flush()
            proc.stdin.close()
    except Exception:
        pass
    try:
        proc.wait(timeout=grace_seconds)
        return
    except subprocess.TimeoutExpired:
        pass

    if os.name == "posix":
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
    else:
        proc.terminate()
    try:
        proc.wait(timeout=grace_seconds)
        return
    except subprocess.TimeoutExpired:
        pass

    if os.name == "posix":
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
    else:
        proc.kill()
    proc.wait(timeout=grace_seconds)


def _run_pattern(args, env, pattern, transport, msg_size, clients):
    if transport == "ipc" and sys.platform.startswith("win"):
        return f"SKIP,current,MULTI_{pattern},{transport},windows_ipc_unsupported"
    if transport not in RUNNABLE_TRANSPORTS.get(pattern, ()):
        return f"UNSUPPORTED,current,MULTI_{pattern},{transport}"
    server_path = ROOT / f"perf_multi_{pattern.lower()}_server.py"
    client_path = ROOT / f"perf_multi_{pattern.lower()}_client.py"
    if not server_path.exists() or not client_path.exists():
        raise SystemExit(f"unsupported pattern: {pattern}")

    server = subprocess.Popen(
        [
            sys.executable,
            str(server_path),
            "--transport",
            transport,
            "--clients",
            clients,
            "--duration",
            args.duration,
            "--msg-size",
            msg_size,
        ],
        cwd=str(ROOT.parent.parent),
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        **_server_popen_kwargs(),
    )
    stdout_chunks = []
    stderr_chunks = []
    client = None
    ready_timeout_s = _arg_or_env_int(
        args.server_ready_timeout_ms,
        "PERF_MULTI_SERVER_READY_TIMEOUT_MS",
        _env_int("PERF_SERVER_READY_TIMEOUT_MS", 10000),
        env,
    ) / 1000.0
    shutdown_grace_s = _arg_or_env_int(
        args.server_shutdown_timeout_ms,
        "PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS",
        _env_int("PERF_SERVER_SHUTDOWN_TIMEOUT_MS", 5000),
        env,
    ) / 1000.0
    timeout_override = _arg_or_env_pair_int(
        "",
        "PERF_MULTI_TIMEOUT_SECONDS",
        "PERF_TIMEOUT_SECONDS",
        0,
        env,
    )
    client_timeout_s = (
        timeout_override
        if timeout_override > 0
        else resolve_multi_timeout_seconds(
            args.duration,
            pattern,
            transport,
            msg_size,
        )
    )
    try:
        ready = _wait_for_control_line(
            server,
            ("READY,", "UNSUPPORTED,", "SKIP,"),
            timeout_s=ready_timeout_s,
            stdout_chunks=stdout_chunks,
        )
        if ready.startswith("UNSUPPORTED,") or ready.startswith("SKIP,"):
            return "\n".join(chunk for chunk in stdout_chunks if chunk)
        if not ready.startswith("READY,"):
            server_stderr = server.stderr.read().strip() if server.stderr else ""
            combined = "\n".join(chunk for chunk in [server_stderr, ready] if chunk)
            if _is_unsupported_output(combined):
                return f"UNSUPPORTED,current,MULTI_{pattern},{transport}"
            raise SystemExit(f"server did not become ready: {combined}")
        endpoint = ready.split(",", 1)[1]
        if pattern == "STREAM":
            # Spawn the shared C perf_stream_client (mirrors the Go/dotnet
            # runner: --pattern MULTI_STREAM so RESULT lines match the
            # Python runner parser; --send-stop-token 1 terminates the
            # Python STREAM server's receive surface).
            stream_client_bin = _ensure_stream_client()
            stream_clients = clients
            non_tcp_max = os.environ.get("PERF_STREAM_NON_TCP_CLIENTS_MAX") or os.environ.get(
                "PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX", "10000"
            )
            if transport != "tcp" and _uint(stream_clients) is not None and _uint(non_tcp_max) is not None:
                stream_clients = str(min(_uint(stream_clients), _uint(non_tcp_max)))
            client_cmd = [
                str(stream_client_bin),
                "--transport",
                transport,
                "--pattern",
                "MULTI_STREAM",
                "--sizes",
                msg_size,
                "--runs",
                "1",
                "--duration",
                args.duration,
                "--ccu",
                stream_clients,
                "--io-threads",
                _effective_role_io_threads(args, "client").split()[0],
                "--completion-wait-ms",
                _stream_completion_wait_ms(),
                "--send-stop-token",
                "1",
                "--endpoint",
                endpoint,
            ]
            try:
                client_run = subprocess.run(
                    client_cmd,
                    cwd=str(REPO_ROOT),
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=client_timeout_s,
                    check=True,
                )
            finally:
                try:
                    server.stdin.write("STOP\n")
                    server.stdin.flush()
                except Exception:
                    pass
            if client_run.stdout:
                stdout_chunks.append(client_run.stdout.strip())
            if client_run.stderr:
                stderr_chunks.append(client_run.stderr.strip())
            try:
                server.wait(timeout=shutdown_grace_s)
            except subprocess.TimeoutExpired:
                _drain_stdout_queue(server, stdout_chunks)
            _drain_stdout_queue(server, stdout_chunks)
            return "\n".join(chunk for chunk in stdout_chunks if chunk)
        one_way_pattern = pattern in {"DEALER_DEALER", "PUBSUB"}
        if one_way_pattern:
            client = subprocess.Popen(
                [
                    sys.executable,
                    str(client_path),
                    "--endpoint",
                    endpoint,
                    "--transport",
                    transport,
                    "--duration",
                    args.duration,
                    "--msg-size",
                    msg_size,
                    "--clients",
                    clients,
                ],
                cwd=str(ROOT.parent.parent),
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                    **_server_popen_kwargs(),
                )
            client_ready = _wait_for_control_line(
                client,
                ("CLIENT_READY,", "UNSUPPORTED,", "SKIP,"),
                timeout_s=client_timeout_s,
                stdout_chunks=stdout_chunks,
            )
            if client_ready.startswith(("UNSUPPORTED,", "SKIP,")):
                return "\n".join(chunk for chunk in stdout_chunks if chunk)
            if client_ready != f"CLIENT_READY,{msg_size}":
                raise SystemExit(f"client did not become ready: {client_ready}")
            server.stdin.write(f"START,{msg_size}\n")
            server.stdin.flush()
            client.stdin.write(f"START,{msg_size}\n")
            client.stdin.flush()
            try:
                client.wait(timeout=client_timeout_s)
            except subprocess.TimeoutExpired as exc:
                _drain_stdout_queue(client, stdout_chunks)
                raise exc
            _drain_stdout_queue(client, stdout_chunks)
            client_stderr = client.stderr.read() if client.stderr else ""
            if client_stderr:
                stderr_chunks.append(client_stderr.strip())
            if client.returncode != 0:
                raise subprocess.CalledProcessError(
                    client.returncode,
                    client.args,
                    output="\n".join(stdout_chunks),
                    stderr=client_stderr,
                )
            try:
                server.wait(timeout=shutdown_grace_s)
            except subprocess.TimeoutExpired:
                _drain_stdout_queue(server, stdout_chunks)
                raise
            _drain_stdout_queue(server, stdout_chunks)
        else:
            client_run = subprocess.run(
                [
                    sys.executable,
                    str(client_path),
                    "--endpoint",
                    endpoint,
                    "--transport",
                    transport,
                    "--duration",
                    args.duration,
                    "--msg-size",
                    msg_size,
                    "--clients",
                    clients,
                ],
                cwd=str(ROOT.parent.parent),
                env=env,
                capture_output=True,
                text=True,
                timeout=client_timeout_s,
                check=True,
            )
            if client_run.stdout:
                stdout_chunks.append(client_run.stdout.strip())
            if client_run.stderr:
                stderr_chunks.append(client_run.stderr.strip())
    except subprocess.CalledProcessError as exc:
        if exc.stdout:
            stdout_chunks.append(_coerce_text(exc.stdout).strip())
        if exc.stderr:
            stderr_chunks.append(_coerce_text(exc.stderr).strip())
        output = "\n".join(chunk for chunk in stderr_chunks + stdout_chunks if chunk)
        if _is_unsupported_output(output):
            return f"UNSUPPORTED,current,MULTI_{pattern},{transport}"
        raise SystemExit(output)
    except subprocess.TimeoutExpired as exc:
        if exc.stdout:
            stdout_chunks.append(_coerce_text(exc.stdout).strip())
        if exc.stderr:
            stderr_chunks.append(_coerce_text(exc.stderr).strip())
        output = "\n".join(
            chunk
            for chunk in stderr_chunks + stdout_chunks + [f"client timed out for pattern {pattern}"]
            if chunk
        )
        if _is_unsupported_output(output):
            return f"UNSUPPORTED,current,MULTI_{pattern},{transport}"
        raise SystemExit(output)
    finally:
        if client is not None and hasattr(client, "poll") and client.poll() is None:
            _terminate_process(client, grace_seconds=shutdown_grace_s)
        _terminate_process(server, grace_seconds=shutdown_grace_s)
        if server.stderr:
            err = server.stderr.read().strip()
            if err:
                stderr_chunks.append(err)
    if stderr_chunks:
        stderr_text = "\n".join(chunk for chunk in stderr_chunks if chunk)
        if _is_unsupported_output(stderr_text):
            return f"UNSUPPORTED,current,MULTI_{pattern},{transport}"
    return "\n".join(chunk for chunk in stdout_chunks if chunk)


def _build_options(args, patterns, transports, requested_msg_sizes, clients, env):
    hwm = args.hwm or "auto-hwm"
    sndhwm = args.send_hwm or args.hwm or "auto-hwm"
    rcvhwm = args.recv_hwm or args.hwm or "auto-hwm"
    sndbuf = args.sndbuf or args.buf or "-1"
    rcvbuf = args.rcvbuf or args.buf or "-1"
    transport_transition_ms = args.transport_transition_ms or _env_pair_value(
        "PERF_MULTI_TRANSPORT_TRANSITION_MS", "PERF_TRANSPORT_TRANSITION_MS", "3000", env
    )
    pattern_transition_ms = args.pattern_transition_ms or _env_pair_value(
        "PERF_MULTI_PATTERN_TRANSITION_MS", "PERF_PATTERN_TRANSITION_MS", "3000", env
    )
    return {
        "lang": "python",
        "suite": "multi",
        "runs": args.runs,
        "patterns": ",".join(f"MULTI_{pattern}" for pattern in patterns),
        "transports": _grouped_option_text(
            patterns,
            lambda pattern: _transports_for_pattern(pattern, transports),
        ),
        "msg_sizes": _grouped_option_text(
            patterns,
            lambda pattern: _msg_sizes_for_pattern(pattern, requested_msg_sizes),
        ),
        "smoke": "1" if args.smoke else "0",
        "duration_seconds": args.duration,
        "clients": clients,
        "default_clients": os.environ.get("PERF_MULTI_DEFAULT_CLIENTS")
        or os.environ.get("PERF_DEFAULT_CLIENTS", "100"),
        "default_stream_clients": os.environ.get("PERF_MULTI_DEFAULT_STREAM_CLIENTS")
        or os.environ.get("PERF_STREAM_DEFAULT_CLIENTS", "10000"),
        "server_io_threads": _effective_role_io_threads(args, "server"),
        "client_io_threads": _effective_role_io_threads(args, "client"),
        "hwm": hwm,
        "sndhwm": sndhwm,
        "rcvhwm": rcvhwm,
        "sndbuf": sndbuf,
        "rcvbuf": rcvbuf,
        "ctx_auto_hwm_enable": os.environ.get("PERF_CTX_AUTO_HWM_ENABLE", "1"),
        "ctx_auto_hwm_profile": args.auto_hwm_profile
        or os.environ.get("PERF_MULTI_CTX_AUTO_HWM_PROFILE")
        or os.environ.get("PERF_CTX_AUTO_HWM_PROFILE", "balanced"),
        "sndtimeo_ms": args.send_timeout_ms or os.environ.get("PERF_MULTI_SNDTIMEO_MS", "200"),
        "rcvtimeo_ms": args.recv_timeout_ms or os.environ.get("PERF_MULTI_RCVTIMEO_MS", "200"),
        "connect_concurrency": args.connect_concurrency or f"{_connect_concurrency_for_clients(clients)} (default)",
        "connect_ready_timeout_ms": args.connect_ready_timeout_ms
        or env.get("PERF_MULTI_CONNECT_READY_TIMEOUT_MS")
        or env.get("PERF_CONNECT_READY_TIMEOUT_MS")
        or "1000",
        "monitor_hwm": args.monitor_hwm or os.environ.get("PERF_MULTI_MONITOR_HWM", "1000"),
        "server_ready_timeout_ms": args.server_ready_timeout_ms
        or env.get("PERF_MULTI_SERVER_READY_TIMEOUT_MS")
        or env.get("PERF_SERVER_READY_TIMEOUT_MS")
        or "10000",
        "server_shutdown_timeout_ms": args.server_shutdown_timeout_ms
        or _env_pair_value("PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS", "PERF_SERVER_SHUTDOWN_TIMEOUT_MS", "5000", env),
        "server_bind_port": args.server_bind_port or os.environ.get("PERF_MULTI_SERVER_BIND_PORT", "0"),
        "transport_transition_ms": transport_transition_ms,
        "pattern_transition_ms": pattern_transition_ms,
        "lat_timeout_ms": os.environ.get("PERF_MULTI_LAT_TIMEOUT_MS", "5000"),
        "stream_non_tcp_clients_max": os.environ.get("PERF_STREAM_NON_TCP_CLIENTS_MAX")
        or os.environ.get("PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX", "10000"),
        "disable_resource_metrics": os.environ.get("PERF_DISABLE_RESOURCE_METRICS", "0"),
        "timeout_seconds": os.environ.get("PERF_MULTI_TIMEOUT_SECONDS")
        or os.environ.get("PERF_TIMEOUT_SECONDS", "auto"),
    }


def _meta_lines(args, clients, runtime_info):
    def git_commit():
        try:
            return subprocess.check_output(
                ["git", "rev-parse", "--short", "HEAD"],
                cwd=str(REPO_ROOT),
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            return "unknown"

    def cpu_name():
        if Path("/proc/cpuinfo").exists():
            for line in Path("/proc/cpuinfo").read_text(errors="ignore").splitlines():
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
        return platform.processor() or "unknown"

    load_avg = " ".join(f"{value:.2f}" for value in os.getloadavg()) if hasattr(os, "getloadavg") else "unknown"
    return [
        f"META,os,{platform.system()} {platform.release()}",
        f"META,cpu,{cpu_name()}",
        f"META,cores,{os.cpu_count() or 0}",
        "META,build,Release",
        f"META,commit,{git_commit()}",
        f"META,timestamp,{datetime.now().astimezone().isoformat(timespec='seconds')}",
        f"META,load_avg,{load_avg}",
        f"META,runs,{args.runs}",
        f"META,clients,{clients}",
        f"META,runtime_libzlink,{runtime_info.path}",
        f"META,runtime_libzlink_sha256,{runtime_info.sha256}",
    ]


def main(argv=None):
    start_time = time.perf_counter()
    args = parse_args(argv or sys.argv[1:])
    _require_binding_runtime()
    if args.pin_cpu and not pin_current_process_cpu0():
        print("warning: cpu pinning requested but could not pin to cpu 0", file=sys.stderr)
    patterns = _parse_patterns(args.pattern)
    transports = _parse_transports(args.transports)
    requested_msg_sizes = _requested_msg_sizes(args)
    clients = _options_clients_display(patterns, args.clients)
    runs = int(args.runs)
    if runs <= 0:
        raise SystemExit("--runs must be > 0")
    configs = _selected_configs(patterns, transports, requested_msg_sizes)

    env = dict(os.environ)
    env["PYTHONPATH"] = str(DEFAULT_PYTHONPATH.resolve())
    env["PERF_MULTI_DURATION_SECONDS"] = str(args.duration)
    if args.io_threads:
        env["PERF_MULTI_SERVER_IO_THREADS"] = args.io_threads
        env["PERF_MULTI_CLIENT_IO_THREADS"] = args.io_threads
    if args.server_io_threads:
        env["PERF_MULTI_SERVER_IO_THREADS"] = args.server_io_threads
    if args.client_io_threads:
        env["PERF_MULTI_CLIENT_IO_THREADS"] = args.client_io_threads
    if args.hwm:
        env["PERF_MULTI_HWM"] = args.hwm
    if args.send_hwm:
        env["PERF_MULTI_SNDHWM"] = args.send_hwm
    if args.recv_hwm:
        env["PERF_MULTI_RCVHWM"] = args.recv_hwm
    if args.buf:
        env["PERF_MULTI_SNDBUF"] = args.buf
        env["PERF_MULTI_RCVBUF"] = args.buf
    if args.sndbuf:
        env["PERF_MULTI_SNDBUF"] = args.sndbuf
    if args.rcvbuf:
        env["PERF_MULTI_RCVBUF"] = args.rcvbuf
    if args.auto_hwm_profile:
        env["PERF_CTX_AUTO_HWM_PROFILE"] = args.auto_hwm_profile
    if args.send_timeout_ms:
        env["PERF_MULTI_SNDTIMEO_MS"] = args.send_timeout_ms
    if args.recv_timeout_ms:
        env["PERF_MULTI_RCVTIMEO_MS"] = args.recv_timeout_ms
    if args.connect_concurrency:
        env["PERF_MULTI_CONNECT_CONCURRENCY"] = args.connect_concurrency
    if args.transport_transition_ms:
        env["PERF_MULTI_TRANSPORT_TRANSITION_MS"] = args.transport_transition_ms
    if args.pattern_transition_ms:
        env["PERF_MULTI_PATTERN_TRANSITION_MS"] = args.pattern_transition_ms
    if args.server_ready_timeout_ms:
        env["PERF_MULTI_SERVER_READY_TIMEOUT_MS"] = args.server_ready_timeout_ms
    if args.connect_ready_timeout_ms:
        env["PERF_MULTI_CONNECT_READY_TIMEOUT_MS"] = args.connect_ready_timeout_ms
    if args.monitor_hwm:
        env["PERF_MULTI_MONITOR_HWM"] = args.monitor_hwm
    if args.server_shutdown_timeout_ms:
        env["PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS"] = args.server_shutdown_timeout_ms
    if args.server_bind_port:
        env["PERF_MULTI_SERVER_BIND_PORT"] = args.server_bind_port
    _apply_dealer_dealer_shutdown_timeout_default(args, env, configs)
    runtime_info = _configure_core_runtime(env)

    run_cooldown_ms = _env_int("PERF_MULTI_RUN_COOLDOWN_MS", _env_int("PERF_RUN_COOLDOWN_MS", 3000))
    transport_transition_ms = _env_int(
        "PERF_MULTI_TRANSPORT_TRANSITION_MS", _env_int("PERF_TRANSPORT_TRANSITION_MS", 3000)
    )
    pattern_transition_ms = _env_int(
        "PERF_MULTI_PATTERN_TRANSITION_MS", _env_int("PERF_PATTERN_TRANSITION_MS", 3000)
    )

    options = _build_options(args, patterns, transports, requested_msg_sizes, clients, env)
    fail_fast = os.environ.get("PERF_FAIL_FAST", "0") == "1"
    options["fail_fast"] = "1" if fail_fast else "0"
    sections = []
    emitted_chunks = []
    status_lines = []
    failures = []
    fail_count = 0
    stop_early = False
    case_ordinal = 1
    for line in _meta_lines(args, clients, runtime_info):
        _append_line(sections, line)
    _append_line(sections)
    _append_line(sections, render_effective_options(options))
    explicit_clients = args.clients is not None or bool(
        os.environ.get("PERF_MULTI_CLIENTS") or os.environ.get("PERF_CLIENTS")
    )
    for pattern_index, pattern in enumerate(patterns):
        if stop_early:
            break
        pattern_clients = _cap_default_clients_for_memory(
            _clients_for_pattern(pattern, args.clients), explicit_clients
        )
        if pattern_index > 0:
            _append_line(sections, "===============================================================================")
            _append_line(sections)
        _append_line(sections, f"## PATTERN: MULTI_{pattern} ({pattern_direction('MULTI_' + pattern)})")
        _append_line(sections, f"  > Benchmarking current for MULTI_{pattern}...")
        pattern_transports = _transports_for_pattern(pattern, transports)
        pattern_msg_sizes = _msg_sizes_for_pattern(pattern, requested_msg_sizes)
        for transport_index, transport in enumerate(pattern_transports):
            if stop_early:
                break
            transport_clients = pattern_clients
            non_tcp_max = os.environ.get("PERF_STREAM_NON_TCP_CLIENTS_MAX") or os.environ.get(
                "PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX", "10000"
            )
            if pattern == "STREAM" and transport != "tcp" and _uint(transport_clients) is not None and _uint(non_tcp_max) is not None:
                transport_clients = str(min(_uint(transport_clients), _uint(non_tcp_max)))
            _append_line(sections, f"    Testing {transport}:")
            transport_failures = 0
            transport_all_unsupported = True
            run_outputs = {msg_size: [] for msg_size in pattern_msg_sizes}
            if runs == 1:
                for header_line in table_header_lines():
                    _append_line(sections, f"      {header_line}")
                for msg_size in pattern_msg_sizes:
                    _append_line(sections, f"    Testing {transport} | {msg_size}B:")
                    resource_skip = _resource_guard_skip(pattern, transport, msg_size, transport_clients)
                    if resource_skip:
                        output = resource_skip
                        emitted_chunks.append(output)
                        status_lines.extend(_parse_status_lines(output))
                        _append_line(sections, _status_row(msg_size, "SKIP"))
                        run_outputs[msg_size].append(output)
                        transport_all_unsupported = False
                        continue
                    case_env = dict(env)
                    case_env["PERF_MULTI_CONNECT_CONCURRENCY"] = args.connect_concurrency or _connect_concurrency_for_clients(transport_clients)
                    case_env["PERF_RUN_ID"] = str(case_ordinal)
                    # C multi default path = context auto-HWM with the raw
                    # socket per-size msg-unit (apply_benchmark_auto_hwm_msg_
                    # unit). Numeric PERF_MULTI_HWM is only honoured under
                    # PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES, so do NOT
                    # force it here.
                    case_env["PERF_MULTI_MSG_UNIT_BYTES"] = str(msg_size)
                    case_ordinal += 1
                    try:
                        output = _run_pattern(
                            args, case_env, pattern, transport, msg_size, transport_clients
                        )
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
                        transport_all_unsupported = False
                        _append_line(sections, _metric_row(pattern, msg_size, metrics))
                    elif status_kind == "unsupported":
                        _append_line(sections, _status_row(msg_size, "N/A"))
                    else:
                        transport_all_unsupported = False
                        if status_kind == "fail":
                            failures.append(
                                f"- MULTI_{pattern} current {transport} {msg_size}B: {_failure_reason(output)}"
                            )
                        _append_line(sections, _status_row(msg_size, "FAIL"))
                    if stop_early:
                        break
            else:
                for run_index in range(runs):
                    if stop_early:
                        break
                    _append_line(sections, f"      run {run_index + 1}/{runs}:")
                    for header_line in table_header_lines():
                        _append_line(sections, f"        {header_line}")
                    for msg_size in pattern_msg_sizes:
                        _append_line(sections, f"      Testing {transport} | {msg_size}B:")
                        resource_skip = _resource_guard_skip(pattern, transport, msg_size, transport_clients)
                        if resource_skip:
                            output = resource_skip
                            emitted_chunks.append(output)
                            status_lines.extend(_parse_status_lines(output))
                            _append_line(sections, _status_row(msg_size, "SKIP", indent="        "))
                            run_outputs[msg_size].append(output)
                            transport_all_unsupported = False
                            continue
                        case_env = dict(env)
                        case_env["PERF_MULTI_CONNECT_CONCURRENCY"] = args.connect_concurrency or _connect_concurrency_for_clients(transport_clients)
                        case_env["PERF_RUN_ID"] = str(case_ordinal)
                        # C multi default path = context auto-HWM with the raw
                        # socket per-size msg-unit. Numeric PERF_MULTI_HWM only
                        # honoured under PERF_MULTI_ALLOW_MANUAL_SOCKET_
                        # OVERRIDES; do NOT force it here.
                        case_env["PERF_MULTI_MSG_UNIT_BYTES"] = str(msg_size)
                        case_ordinal += 1
                        try:
                            output = _run_pattern(
                                args,
                                case_env,
                                pattern,
                                transport,
                                msg_size,
                                transport_clients,
                            )
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
                            transport_all_unsupported = False
                            _append_line(
                                sections,
                                _metric_row(pattern, msg_size, metrics, indent="        "),
                            )
                        elif status_kind == "unsupported":
                            _append_line(
                                sections,
                                _status_row(msg_size, "N/A", indent="        "),
                            )
                        else:
                            transport_all_unsupported = False
                            if status_kind == "fail":
                                failures.append(
                                    f"- MULTI_{pattern} current {transport} {msg_size}B: {_failure_reason(output)}"
                                )
                            _append_line(
                                sections,
                                _status_row(msg_size, "FAIL", indent="        "),
                            )
                        if stop_early:
                            break
                    if run_index + 1 < runs:
                        if stop_early:
                            break
                        _append_line(sections, f"      [cooldown {run_cooldown_ms}ms]")
                        time.sleep(run_cooldown_ms / 1000.0)
                _append_line(sections, "      median:")
                for header_line in table_header_lines():
                    _append_line(sections, f"        {header_line}")
                for msg_size in pattern_msg_sizes:
                    metrics = _median_metrics(
                        run_outputs[msg_size], pattern, transport, msg_size
                    )
                    if metrics:
                        transport_all_unsupported = False
                        _append_line(
                            sections,
                            _metric_row(pattern, msg_size, metrics, indent="        "),
                        )
                    elif any(_status_kind(output) == "unsupported" for output in run_outputs[msg_size]):
                        _append_line(
                            sections,
                            _status_row(msg_size, "N/A", indent="        "),
                        )
                    else:
                        transport_all_unsupported = False
                        _append_line(
                            sections,
                            _status_row(msg_size, "FAIL", indent="        "),
                        )
            if transport_all_unsupported:
                suffix = "unsupported Done"
            elif transport_failures:
                suffix = f"(failures={transport_failures}) Done"
            else:
                suffix = "Done"
            _append_line(sections, f"    Testing {transport}: {suffix}")
            for line in multi_auto_hwm_lines(pattern, pattern_msg_sizes):
                _append_line(sections, line)
            if transport_index + 1 < len(pattern_transports):
                if stop_early:
                    break
                _append_line(
                    sections,
                    f"    [transport cooldown {transport_transition_ms}ms]",
                )
                time.sleep(transport_transition_ms / 1000.0)
        if not stop_early and pattern_index + 1 < len(patterns):
            time.sleep(pattern_transition_ms / 1000.0)
        _append_line(sections)
    rows = parse_result_lines("\n".join(emitted_chunks), warn=_warn_runner)
    emitted_result_lines = [
        line
        for chunk in emitted_chunks
        for line in chunk.splitlines()
        if line.startswith("RESULT,")
    ]
    emitted_result_lines = sort_result_data_lines(emitted_result_lines)
    skipped_cases = 0
    unsupported_cases = 0
    skip_entries = []
    for line in status_lines:
        if line.startswith("SKIP,"):
            skipped_cases += 1
            parts = line.split(",", 4)
            label = " ".join(part for part in parts[2:4] if part)
            reason = parts[4] if len(parts) > 4 else "skip"
            skip_entries.append((label, reason))
        elif line.startswith("UNSUPPORTED,"):
            unsupported_cases += 1
    total_cases = len(configs) * runs
    expected_cases = max(0, total_cases - skipped_cases - unsupported_cases)
    expected_result_lines = expected_cases * 5
    status = "complete" if len(rows) == expected_result_lines else "partial"
    success_cases = len(rows) // 5
    _append_line(sections, render_effective_options(options, section="result"))
    if emitted_result_lines:
        _append_line(sections)
        _append_line(sections, "## Result Data")
        for line in emitted_result_lines:
            _append_line(sections, line)
    _append_line(sections)
    _append_line(sections, "## Completion")
    _append_line(sections, f"- success: {success_cases}")
    _append_line(sections, f"- unsupported: {unsupported_cases}")
    _append_line(sections, f"- skip: {skipped_cases}")
    _append_line(sections, f"- fail: {fail_count}")
    _append_line(sections, f"- status: {status}")
    _append_line(sections, f"- expected_result_lines: {expected_result_lines}")
    _append_line(sections, f"- actual_result_lines: {len(rows)}")
    if skip_entries:
        _append_line(sections)
        _append_line(sections, "## Skips")
        for label, reason in skip_entries:
            _append_line(sections, f"- {label}: {reason}")
    if failures:
        _append_line(sections)
        _append_line(sections, "## Failures")
        for line in failures:
            _append_line(sections, line)
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
        suite="multi",
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
