import argparse
import os
import sys
import time
from pathlib import Path

PERF_DIR = Path(__file__).resolve().parent.parent
perf_dir_text = str(PERF_DIR)
if perf_dir_text not in sys.path:
    sys.path.insert(0, perf_dir_text)

from perf_stop_token import (
    STOP_TOKEN,
    is_stop_token,
    is_stop_token_in_parts,
)

from perf_metrics import (
    HEADER_MAGIC,
    HEADER_SIZE,
    benchmark_run_id,
    build_report_path,
    configure_tls_client,
    configure_tls_server,
    decode_header,
    extract_metric_payload,
    latency_ns_from_message,
    new_payload,
    is_active_message,
    payload_phase,
    parse_result_lines,
    pin_current_process_cpu0,
    print_result_lines,
    resolve_multi_connect_ready_timeout_ms,
    render_effective_options,
    render_markdown_summary,
    result_metrics,
    rows_by_case,
    safe_poll,
    stamp_payload,
    status_row_text,
    table_header_lines,
    throughput_unit,
    tcp_endpoint,
    transport_endpoint,
    wait_monitor_event,
    _require_zlink,
)


TOPIC = b"bench"
PYTHON_MULTI_DEFAULT_IO_THREADS = 4


def received_has_stop_token(received):
    for part in received:
        data = part.data
        if len(data) == len(STOP_TOKEN) and data == STOP_TOKEN:
            return True
    return False


def received_metric_payload(received, *, expected_size=None):
    if not received:
        return memoryview(b"")
    return metric_payload_data(received.parts[-1].data, expected_size=expected_size)


def metric_payload_data(data, *, expected_size=None):
    if expected_size is not None and len(data) != expected_size:
        return memoryview(b"")
    header = decode_header(data)
    if header is not None and header["magic"] == HEADER_MAGIC:
        return data
    return memoryview(b"")


def parse_client_args(argv, *, pattern):
    parser = argparse.ArgumentParser(prog=f"perf_multi_{pattern.lower()}_client.py")
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--transport", default="tcp")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--msg-size", type=int, default=64)
    parser.add_argument("--clients", type=int, default=100)
    args = parser.parse_args(argv)
    if args.duration <= 0 or args.msg_size < HEADER_SIZE or args.clients <= 0:
        raise SystemExit("invalid perf arguments")
    args.transport = args.transport.lower()
    return args


def parse_server_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint")
    parser.add_argument("--transport", default="tcp")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--msg-size", type=int, default=64)
    args = parser.parse_args(argv)
    args.transport = args.transport.lower()
    if args.duration <= 0 or args.msg_size < HEADER_SIZE or args.clients <= 0:
        raise SystemExit("invalid perf arguments")
    return args


def _env_int(name, default):
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    try:
        return int(value)
    except ValueError:
        return default


def _perf_context(primary_env):
    zlink_mod = _require_zlink()
    ctx = zlink_mod.create_context()
    default_io_threads = _env_int(
        "PERF_MULTI_DEFAULT_IO_THREADS",
        _env_int("PERF_DEFAULT_IO_THREADS", PYTHON_MULTI_DEFAULT_IO_THREADS),
    )
    io_threads = _env_int(
        primary_env, _env_int("PERF_IO_THREADS", default_io_threads)
    )
    if io_threads > 0:
        ctx.options.io_threads = io_threads
    return ctx


def perf_server_context():
    return _perf_context("PERF_MULTI_SERVER_IO_THREADS")


def perf_client_context():
    return _perf_context("PERF_MULTI_CLIENT_IO_THREADS")


def _env_flag(name):
    value = os.environ.get(name)
    return value is not None and value != "" and value == "1"


def bench_multi_manual_socket_overrides_allowed():
    # C perf_multi_runtime.hpp bench_manual_socket_overrides_allowed: numeric
    # HWM only applied under these env flags; default path = context auto-HWM.
    return _env_flag("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES") or _env_flag(
        "PERF_ALLOW_MANUAL_SOCKET_OVERRIDES"
    )


def resolve_multi_send_hwm():
    return _env_int("PERF_MULTI_SNDHWM", _env_int("PERF_MULTI_HWM", 0))


def resolve_multi_recv_hwm():
    return _env_int("PERF_MULTI_RCVHWM", _env_int("PERF_MULTI_HWM", 0))


def resolve_multi_send_timeout_ms():
    return _env_int("PERF_MULTI_SNDTIMEO_MS", 200)


def resolve_multi_recv_timeout_ms():
    return _env_int("PERF_MULTI_RCVTIMEO_MS", 200)





def resolve_multi_server_ready_timeout_ms():
    return _env_int("PERF_MULTI_SERVER_READY_TIMEOUT_MS", _env_int("PERF_SERVER_READY_TIMEOUT_MS", 10000))


def resolve_multi_server_shutdown_timeout_ms():
    return _env_int("PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS", _env_int("PERF_SERVER_SHUTDOWN_TIMEOUT_MS", 5000))


def resolve_multi_timeout_seconds(duration_seconds, pattern, transport, msg_size):
    override = _env_int("PERF_MULTI_TIMEOUT_SECONDS", _env_int("PERF_TIMEOUT_SECONDS", 0))
    if override > 0:
        return override
    size = max(int(msg_size), 64)
    duration = max(float(duration_seconds), 1.0)
    if pattern == "STREAM":
        return max(45, int(duration * 3.0) + 20)
    if transport in {"tls", "wss"} and size >= 131072:
        return max(90, int(duration * 6.0) + 30)
    return max(45, int(duration * 3.0) + 20)


def configure_multi_tls_server(target, transport):
    configure_tls_server(target, transport)


def configure_multi_tls_client(target, transport):
    configure_tls_client(target, transport)


def apply_multi_socket_options(*sockets, receive_timeout_ms=None):
    # C perf_multi_runtime.hpp apply_benchmark_hwm: numeric SNDHWM/RCVHWM only
    # under PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES. Default path leaves the
    # context auto-HWM in effect.
    overrides = bench_multi_manual_socket_overrides_allowed()
    send_hwm = resolve_multi_send_hwm()
    recv_hwm = resolve_multi_recv_hwm()
    send_timeout_ms = resolve_multi_send_timeout_ms()
    recv_timeout_ms = (
        resolve_multi_recv_timeout_ms()
        if receive_timeout_ms is None
        else receive_timeout_ms
    )
    for sock in sockets:
        sock.options.linger_ms = 0
        if overrides:
            if send_hwm > 0:
                sock.options.send_high_water_mark = send_hwm
            if recv_hwm > 0:
                sock.options.receive_high_water_mark = recv_hwm
        sock.options.send_timeout_ms = send_timeout_ms
        sock.options.receive_timeout_ms = recv_timeout_ms


def apply_multi_auto_hwm_msg_unit(ctx, msg_size):
    if msg_size <= 0:
        return
    ctx.options.auto_hwm_msg_unit_bytes = msg_size












def recv_nonblocking(sock, *, method="recv", storage=None):
    zlink_mod = _require_zlink()
    if method == "recv":
        recv_method = sock.recv_into
        if storage is None:
            storage = zlink_mod.create_received()
    elif method == "subscribe":
        recv_method = sock.subscribe_into
        if storage is None:
            storage = zlink_mod.create_topic_message()
    else:
        raise ValueError(f"unsupported recv method: {method}")
    try:
        return storage if recv_method(storage, flags=zlink_mod.RecvFlags.DONT_WAIT) else None
    except zlink_mod.RecvError as exc:
        if exc.result == zlink_mod.RecvResult.NO_DATA:
            return None
        raise


_DONT_WAIT_FLAG = None
_SUBMIT_ERROR = None
_SUBMIT_BACKPRESSURED = None


def _dont_wait_flag():
    global _DONT_WAIT_FLAG
    if _DONT_WAIT_FLAG is None:
        _DONT_WAIT_FLAG = int(_require_zlink().SendFlags.DONT_WAIT)
    return _DONT_WAIT_FLAG


def _submit_error_type():
    global _SUBMIT_ERROR
    if _SUBMIT_ERROR is None:
        _SUBMIT_ERROR = _require_zlink().SubmitError
    return _SUBMIT_ERROR


def _submit_backpressured_result():
    global _SUBMIT_BACKPRESSURED
    if _SUBMIT_BACKPRESSURED is None:
        _SUBMIT_BACKPRESSURED = _require_zlink().SubmitResult.BACKPRESSURED
    return _SUBMIT_BACKPRESSURED


def send_nonblocking(sock, payload, *, method="send", routing_id=None):
    if method != "send":
        raise ValueError(f"unsupported send method: {method}")
    send_method = sock.send
    flag = _dont_wait_flag()
    try:
        if routing_id is None:
            op = send_method().flags(flag)
        else:
            op = send_method(routing_id).flags(flag)
        if isinstance(payload, (list, tuple)):
            op.messages(*payload)
        else:
            op.message(payload)
        return bool(op.submit())
    except _submit_error_type() as exc:
        if exc.result == _submit_backpressured_result():
            return False
        raise



def publish_nonblocking(sock, topic, payload):
    flag = _dont_wait_flag()
    try:
        op = sock.publish(topic).flags(flag)
        if isinstance(payload, (list, tuple)):
            op.messages(*payload)
        else:
            op.message(payload)
        return bool(op.submit())
    except _submit_error_type() as exc:
        if exc.result == _submit_backpressured_result():
            return False
        raise



def wait_for_command_line(stream, *, deadline):
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            return None
        line = stream.readline()
        if not line:
            return None
        text = line.strip()
        if text:
            return text



def benchmark_endpoint(transport, prefix):
    endpoint = transport_endpoint(transport, prefix)
    bind_port = _env_int("PERF_MULTI_SERVER_BIND_PORT", 0)
    if bind_port <= 0:
        return endpoint
    scheme = transport.lower()
    if scheme not in {"tcp", "tls", "ws", "wss"}:
        return endpoint
    return f"{scheme}://127.0.0.1:{bind_port}"
