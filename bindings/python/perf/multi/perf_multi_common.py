import argparse
import asyncio
import os
import sys
import time
from contextlib import contextmanager
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
    active_message_latency_ns,
    HEADER_MAGIC,
    HEADER_SIZE,
    LatencySampler,
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
PYTHON_MULTI_DEFAULT_IO_THREADS = 1
PERF_MULTI_AUX_POLL_WAIT_MS = 100
RELAY_SCHEDULER_QUANTUM = 32


class RelaySchedulerQuantum:
    """Bound cooperative scheduling latency without limiting admissions."""

    __slots__ = ("_processed", "quantum")

    def __init__(self, quantum=RELAY_SCHEDULER_QUANTUM):
        if quantum <= 0:
            raise ValueError("relay scheduler quantum must be positive")
        self.quantum = quantum
        self._processed = 0

    def received(self):
        self._processed += 1
        if self._processed < self.quantum:
            return False
        self._processed = 0
        return True

    def reset(self):
        self._processed = 0


@contextmanager
def scoped_relay_eager_task_factory(loop=None):
    """Use Python 3.12 eager task start only within the relay hot loop."""

    loop = asyncio.get_running_loop() if loop is None else loop
    previous_factory = loop.get_task_factory()
    eager_factory = getattr(asyncio, "eager_task_factory", None)
    if eager_factory is not None:
        loop.set_task_factory(eager_factory)
    try:
        yield eager_factory is not None
    finally:
        if eager_factory is not None:
            loop.set_task_factory(previous_factory)


def make_relay_send_done_callback(pending_tasks, send_errors):
    """Create one completion classifier shared by every relay reply task."""

    zlink_mod = _require_zlink()
    ignored_results = {
        zlink_mod.SubmitResult.NOT_CONNECTED,
        zlink_mod.SubmitResult.NOT_FOUND,
    }

    def on_send_done(task):
        pending_tasks.discard(task)
        if task.cancelled():
            return
        exc = task.exception()
        if isinstance(exc, zlink_mod.SubmitError) and exc.result in ignored_results:
            return
        if exc is not None:
            send_errors.append(exc)

    return on_send_done


def track_relay_send_task(task, pending_tasks, on_send_done):
    """Track only genuinely pending tasks; classify eager completions inline."""

    if task.done():
        on_send_done(task)
        return False
    pending_tasks.add(task)
    task.add_done_callback(on_send_done)
    return True


def received_has_stop_token(received):
    parts = received.parts
    if len(parts) != 1:
        return False
    data = parts[0].data
    return len(data) == len(STOP_TOKEN) and data == STOP_TOKEN


def received_metric_payload(received, *, expected_size=None):
    if not received:
        return memoryview(b"")
    parts = received.parts
    expected_count = measurement_part_count()
    if len(parts) != expected_count or (expected_count == 2 and len(parts[1].data) != 0):
        return memoryview(b"")
    return metric_payload_data(parts[0].data, expected_size=expected_size)


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


def resolve_multi_monitor_hwm_bytes(environment=None):
    environment = os.environ if environment is None else environment
    for name in ("PERF_MULTI_MONITOR_HWM", "PERF_MONITOR_HWM"):
        value = environment.get(name)
        if value in (None, ""):
            continue
        try:
            parsed = int(value)
        except ValueError:
            continue
        if parsed >= 0:
            return parsed
    return 4_096_000


def resolve_multi_reqrep_timeout_ms():
    return _env_int("PERF_MULTI_REQREP_TIMEOUT_MS", 200)


def resolve_multi_reqrep_drain_timeout_ms():
    request_timeout_ms = resolve_multi_reqrep_timeout_ms()
    return _env_int(
        "PERF_MULTI_REQREP_DRAIN_TIMEOUT_MS",
        max(1000, request_timeout_ms * 4),
    )





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


_SUBMIT_ERROR = None
_SUBMIT_BACKPRESSURED = None


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


def measurement_part_count():
    return 1 if os.environ.get("PERF_PART_COUNT") == "1" else 2


def measurement_parts(payload):
    return (payload,) if measurement_part_count() == 1 else (payload, b"")


async def send_routed(
    sock,
    payload,
    *,
    routing_id=None,
    measurement=True,
    method="send",
    _yield_after_submit=True,
    _sync=False,
    _deadline=None,
):
    while True:
        try:
            send_method = getattr(sock, method)
            op = send_method() if routing_id is None else send_method(routing_id)
            if measurement and not isinstance(payload, (list, tuple)):
                op.messages(*measurement_parts(payload))
            elif isinstance(payload, (list, tuple)):
                op.messages(*payload)
            else:
                op.message(payload)
            if _sync:
                op.submit_sync()
            else:
                await op.submit()
            # Core may admit inline (op_id == 0), in which case awaiting the
            # public terminal does not suspend this coroutine. Always yield
            # one turn for continuous send loops so receive progress cannot
            # be starved for the entire active phase. One-shot routed echo
            # tasks already have a scheduling yield at their creation site
            # and opt out to avoid yielding twice for one reply.
            if _yield_after_submit:
                await asyncio.sleep(0)
            return True
        except _submit_error_type() as exc:
            if exc.result != _submit_backpressured_result():
                raise
            if _deadline is not None and time.perf_counter() >= _deadline:
                return False
            # No operation was accepted, so rebuild the public builder after
            # one cooperative event-loop turn. Core/HWM still owns depth;
            # this adds no retry timer, application window, or pending queue.
            await asyncio.sleep(0)



def publish_sync(sock, topic, payload, *, measurement=True):
    op = sock.publish(topic)
    if measurement and not isinstance(payload, (list, tuple)):
        op.messages(*measurement_parts(payload))
    elif isinstance(payload, (list, tuple)):
        op.messages(*payload)
    else:
        op.message(payload)
    op.submit()



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
    if transport.lower() == "ipc":
        sock_path = Path("/tmp") / (
            f"zlink-python-perf-{prefix}-{os.getpid()}-{time.time_ns()}.ipc"
        )
        return f"ipc://{sock_path}"
    endpoint = transport_endpoint(transport, prefix)
    bind_port = _env_int("PERF_MULTI_SERVER_BIND_PORT", 0)
    if bind_port <= 0:
        return endpoint
    scheme = transport.lower()
    if scheme not in {"tcp", "tls", "ws", "wss"}:
        return endpoint
    return f"{scheme}://127.0.0.1:{bind_port}"
