import argparse
import os
import struct
import sys
import threading
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

_idle_wait_local = threading.local()

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
    parse_result_lines,
    print_result_lines,
    is_active_message,
    payload_phase,
    pin_current_process_cpu0,
    render_effective_options,
    render_markdown_summary,
    result_metrics,
    resolve_single_connect_ready_timeout_ms,
    resolve_single_timeout_seconds,
    rows_by_case,
    safe_poll,
    stamp_payload,
    status_row_text,
    table_header_lines,
    throughput_unit,
    transport_endpoint,
    wait_monitor_event,
    _require_zlink,
)


def parse_single_args(argv, *, pattern):
    parser = argparse.ArgumentParser(prog=f"perf_{pattern.lower()}.py")
    parser.add_argument("--transport", default="tcp")
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--msg-size", type=int, default=256)
    args = parser.parse_args(argv)
    if args.duration <= 0:
        raise SystemExit("--duration must be > 0")
    if args.msg_size < HEADER_SIZE:
        raise SystemExit(f"--msg-size must be >= {HEADER_SIZE}")
    args.transport = args.transport.lower()
    return args


def _env_int(name, default):
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    try:
        return int(value)
    except ValueError:
        return default


def perf_context():
    zlink_mod = _require_zlink()
    ctx = zlink_mod.create_context()
    io_threads = _env_int("PERF_IO_THREADS", 1)
    if io_threads > 0:
        ctx.options.io_threads = io_threads
    return ctx


def poll_idle_ms(timeout_ms=1):
    timeout_ms = max(1, int(timeout_ms))
    state = getattr(_idle_wait_local, "state", None)
    if state is None:
        zlink_mod = _require_zlink()
        timer = zlink_mod.create_timer()
        poller = zlink_mod.create_poller()
        events = zlink_mod.create_poll_events(1)
        poller.add_timer(timer, 0)
        state = (timer, poller, events)
        _idle_wait_local.state = state
    timer, poller, events = state
    while timer.recv() is not None:
        pass
    timer.start(timeout_ms * 1_000_000, 1)
    safe_poll(poller, events, timeout_ms + 1)
    while timer.recv() is not None:
        pass


def _env_flag(name):
    value = os.environ.get(name)
    return value is not None and value != "" and value == "1"


def bench_single_manual_socket_overrides_allowed():
    # C bench_common_runtime.hpp bench_single_manual_socket_overrides_allowed:
    # numeric HWM/buffers are gated behind these env flags; default path uses
    # context auto-HWM only.
    return _env_flag("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES") or _env_flag(
        "PERF_ALLOW_MANUAL_SOCKET_OVERRIDES"
    )


def resolve_single_send_hwm():
    return _env_int("PERF_SINGLE_SNDHWM", _env_int("PERF_SINGLE_HWM", 0))


def resolve_single_recv_hwm():
    return _env_int("PERF_SINGLE_RCVHWM", _env_int("PERF_SINGLE_HWM", 0))


def resolve_single_send_timeout_ms():
    return _env_int("PERF_SINGLE_SNDTIMEO_MS", -1)


def resolve_single_recv_timeout_ms():
    return _env_int("PERF_SINGLE_RCVTIMEO_MS", 200)


def resolve_single_pubsub_recv_timeout_ms():
    return _env_int(
        "PERF_SINGLE_PUBSUB_RCVTIMEO_MS",
        resolve_single_recv_timeout_ms(),
    )


def resolve_single_pubsub_ready_settle_s():
    return _env_int("PERF_SINGLE_PUBSUB_READY_SETTLE_MS", 1000) / 1000.0



def resolve_single_endpoint(transport, prefix):
    return transport_endpoint(transport, prefix)


def configure_single_tls_server(target, transport):
    configure_tls_server(target, transport)


def configure_single_tls_client(target, transport):
    configure_tls_client(target, transport)


def apply_single_socket_options(*sockets, receive_timeout_ms=None):
    # C bench_common_runtime.hpp apply_single_hwm: numeric SNDHWM/RCVHWM only
    # applied under PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES; the default path
    # relies on context auto-HWM. apply_single_benchmark_socket_options always
    # sets linger=0 and the recv/send timeouts.
    overrides = bench_single_manual_socket_overrides_allowed()
    send_hwm = resolve_single_send_hwm()
    recv_hwm = resolve_single_recv_hwm()
    send_timeout_ms = resolve_single_send_timeout_ms()
    recv_timeout_ms = (
        resolve_single_recv_timeout_ms()
        if receive_timeout_ms is None
        else receive_timeout_ms
    )
    for sock in sockets:
        sock.options.linger_ms = 0
        # bindings/cpp/perf single perf_pair.cpp et al set tcp_no_delay on
        # the raw single sockets so 64B one-way throughput is not pinned by
        # Nagle coalescing. Best-effort: ignore on transports without it.
        try:
            sock.options.tcp_no_delay = True
        except Exception:
            pass
        if overrides:
            if send_hwm > 0:
                sock.options.send_high_water_mark = send_hwm
            if recv_hwm > 0:
                sock.options.receive_high_water_mark = recv_hwm
        if send_timeout_ms >= 0:
            sock.options.send_timeout_ms = send_timeout_ms
        sock.options.receive_timeout_ms = recv_timeout_ms


def apply_single_auto_hwm_msg_unit(ctx, msg_size):
    # Context-level message unit follows the current payload size; numeric
    # socket HWM remains behind the manual-override gate.
    if msg_size <= 0:
        return
    ctx.options.auto_hwm_msg_unit_bytes = msg_size



def _recv_storage(method):
    zlink_mod = _require_zlink()
    if method == "recv":
        return zlink_mod.create_received()
    if method == "subscribe":
        return zlink_mod.create_topic_message()
    raise ValueError(f"unsupported recv method: {method}")


def recv_nonblocking(sock, *, method="recv"):
    zlink_mod = _require_zlink()
    recv_method = sock.subscribe_into if method == "subscribe" else sock.recv_into
    storage = _recv_storage(method)
    try:
        return storage if recv_method(storage, flags=zlink_mod.RecvFlags.DONT_WAIT) else None
    except zlink_mod.RecvError as exc:
        if exc.result == zlink_mod.RecvResult.NO_DATA:
            return None
        raise


def storage_data_part(storage):
    """Return the bytes of the final (data) frame of a received message
    without materializing every frame via to_bytes_list(). The metric
    payload is always the last frame (C recv_single_part_header_flags
    rejects extra/routing frames); router prepends only a routing frame
    as binding metadata, never as a data frame here."""

    parts = storage.parts
    if not parts:
        return b""
    return parts[-1].to_bytes()


def recv_into_storage(sock, storage, *, method="recv", blocking=True):
    """C perf_single_one_way.hpp recv idiom: blocking first recv then a
    DONTWAIT burst-drain, both reusing one caller-owned storage object
    (no poller, no per-message storage allocation). Returns True on a
    received message, False on EAGAIN/NO_DATA."""

    zlink_mod = _require_zlink()
    recv_method = sock.subscribe_into if method == "subscribe" else sock.recv_into
    flags = 0 if blocking else int(zlink_mod.RecvFlags.DONT_WAIT)
    try:
        return bool(recv_method(storage, flags=flags))
    except zlink_mod.RecvError as exc:
        if exc.result == zlink_mod.RecvResult.NO_DATA:
            return False
        raise


def run_one_way_receiver(sock, *, method, msg_size, run_id, active_end,
                         received, latencies):
    """C perf_single_one_way.hpp run_active_phase receiver, fused for the
    Python hot path on one reused storage object. Python uses DONTWAIT for
    the receive loop so a lost stop token cannot leave the runner blocked
    inside native recv. Header is decoded strictly at offset 0 with an exact
    size check; latency = recv_ns - sent_ts_ns clamped to 0.0. Returns the
    updated received count."""

    from perf_metrics import (
        HEADER_FORMAT,
        HEADER_MAGIC,
        HEADER_SIZE,
    )

    zlink_mod = _require_zlink()
    dont_wait = int(zlink_mod.RecvFlags.DONT_WAIT)
    recv_error = zlink_mod.RecvError
    no_data = zlink_mod.RecvResult.NO_DATA
    storage = _recv_storage(method)
    poller, poll_events = new_socket_poller(sock, zlink_mod.PollEventFlag.POLLIN)
    perf_counter = time.perf_counter
    time_ns = time.time_ns
    count = received
    stop_view = memoryview(STOP_TOKEN)
    stop_wait_end = active_end + (_env_int("PERF_SINGLE_STOP_WAIT_MS", 2000) / 1000.0)

    try:
        stop_received = False
        while not stop_received:
            if perf_counter() >= stop_wait_end:
                break
            flags = dont_wait
            while True:
                try:
                    recv_method = (
                        sock.subscribe_into if method == "subscribe" else sock.recv_into
                    )
                    if recv_method(storage, flags=flags):
                        parts = storage.to_bytes_list()
                        data = parts[-1] if parts else b""
                    else:
                        wait_socket_readable_until(poller, poll_events, stop_wait_end)
                        break
                except recv_error as exc:
                    if exc.result == no_data:
                        wait_socket_readable_until(poller, poll_events, stop_wait_end)
                        break
                    raise
                if perf_counter() >= stop_wait_end:
                    stop_received = True
                    break
                flags = dont_wait
                if len(data) == len(stop_view) and data == stop_view:
                    stop_received = True
                    break
                if len(data) != msg_size or len(data) < HEADER_SIZE:
                    continue
                magic, hdr_run_id, phase, hdr_msg_size, _seq, sent_ts_ns = (
                    struct.unpack_from(HEADER_FORMAT, data, 0)
                )
                if (
                    magic != HEADER_MAGIC
                    or phase != 1
                    or hdr_msg_size != msg_size
                    or hdr_run_id != run_id
                ):
                    continue
                if perf_counter() >= active_end:
                    continue
                count += 1
                now_ns = time_ns()
                if sent_ts_ns > 0 and now_ns >= sent_ts_ns:
                    latencies.append(float(now_ns - sent_ts_ns))
                else:
                    latencies.append(0.0)
    finally:
        poller.close()
    return count


def run_one_way_receiver_public_recv(
    sock, *, msg_size, run_id, active_end, received, latencies
):
    return run_one_way_receiver(
        sock,
        method="recv",
        msg_size=msg_size,
        run_id=run_id,
        active_end=active_end,
        received=received,
        latencies=latencies,
    )


def run_one_way_subscriber_public_subscribe(
    sock, *, topic, msg_size, run_id, active_end, received, latencies
):
    return run_one_way_receiver(
        sock,
        method="subscribe",
        msg_size=msg_size,
        run_id=run_id,
        active_end=active_end,
        received=received,
        latencies=latencies,
    )


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


def send_nonblocking(sock, payload, *, routing_id=None):
    flag = _dont_wait_flag()
    try:
        if routing_id is None:
            op = sock.send()
        else:
            op = sock.send(routing_id)
        return bool(op.message(payload).flags(flag).submit())
    except _submit_error_type() as exc:
        if exc.result == _submit_backpressured_result():
            return False
        raise


def publish_nonblocking(sock, topic, payload):
    flag = _dont_wait_flag()
    try:
        return bool(
            sock.publish(topic)
            .message(payload)
            .flags(flag)
            .submit()
        )
    except _submit_error_type() as exc:
        if exc.result == _submit_backpressured_result():
            return False
        raise


def new_socket_poller(sock, events):
    zlink_mod = _require_zlink()
    poller = zlink_mod.create_poller()
    poll_events = zlink_mod.create_poll_events(1)
    poller.add_socket(sock, events, 0)
    return poller, poll_events


def wait_socket_readable_until(poller, events, deadline):
    remaining_s = deadline - time.perf_counter()
    if remaining_s <= 0:
        return
    wait_ms = max(1, int(min(remaining_s, 0.050) * 1000))
    safe_poll(poller, events, wait_ms)


def single_routing_probe(sender, receiver, payload, *, run_id, msg_size,
                         routing_id=None):
    """C perf_dealer_router.cpp wait_for_dealer_router_ready /
    perf_router_router.cpp wait_for_router_router_ready: one-shot routing
    self-check. Bounded by PERF_CONNECT_READY_TIMEOUT_MS; DONTWAIT probe
    sends (phase=active, seq=0) until the receiver confirms one matching
    header. No retry/sleep loop beyond C's bounded probe."""

    from perf_metrics import decode_header as _decode_header

    deadline = time.perf_counter() + (
        resolve_single_connect_ready_timeout_ms() / 1000.0
    )
    poller, poll_events = new_socket_poller(receiver, _require_zlink().PollEventFlag.POLLIN)
    probe = stamp_payload(payload, phase=1, run_id=run_id, seq=0)
    try:
        while time.perf_counter() < deadline:
            send_nonblocking(sender, probe, routing_id=routing_id)
            probe_deadline = min(deadline, time.perf_counter() + 0.05)
            while time.perf_counter() < probe_deadline:
                received = recv_nonblocking(receiver)
                if received is None:
                    wait_socket_readable_until(poller, poll_events, probe_deadline)
                    continue
                with received:
                    parts = received.to_bytes_list()
                data = extract_metric_payload(parts)
                header = _decode_header(data)
                if (
                    header is not None
                    and header["magic"] == HEADER_MAGIC
                    and header["run_id"] == run_id
                    and header["phase"] == 1
                    and header["msg_size"] == msg_size
                ):
                    return True
    finally:
        poller.close()
    return False
