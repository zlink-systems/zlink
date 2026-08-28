import threading
import time

import zlink

from perf_common import (
    STOP_TOKEN,
    apply_single_socket_options,
    benchmark_run_id,
    configure_single_tls_client,
    configure_single_tls_server,
    measurement_parts,
    measurement_payload,
    new_payload,
    parse_single_args,
    perf_context,
    print_result_lines,
    resolve_single_connect_ready_timeout_ms,
    resolve_single_endpoint,
    resolve_single_latency_sample_cap,
    resolve_single_reqrep_drain_timeout_ms,
    resolve_single_reqrep_timeout_ms,
    result_metrics,
    stamp_payload,
    wait_monitor_event,
)
from perf_metrics import HEADER_MAGIC, LatencySampler, decode_header


_PROBE_TOKEN = b"__zlink_perf_reqrep_probe__"


def _transient_submit_result(result):
    return result in (
        zlink.SubmitResult.BACKPRESSURED,
        zlink.SubmitResult.NOT_CONNECTED,
        zlink.SubmitResult.NOT_FOUND,
    )


def _close_messages(parts):
    for part in parts:
        part.close()


def _reply_parts(received, parts, drain_timeout_s):
    deadline = time.perf_counter() + drain_timeout_s
    while time.perf_counter() < deadline:
        try:
            received.reply().messages(*parts).submit()
            return True
        except zlink.SubmitError as exc:
            if not _transient_submit_result(exc.result):
                raise
    return False


def _run_replier(replier, state, drain_timeout_s):
    received = zlink.create_received()
    try:
        while not state["stop"]:
            try:
                if not replier.recv_into(received):
                    continue
            except zlink.RecvError as exc:
                if exc.result == zlink.RecvResult.NO_DATA:
                    continue
                raise

            try:
                parts = received.to_bytes_list()
                if len(parts) == 1 and parts[0] == STOP_TOKEN:
                    return
                if received.routing_id is None or received.request_seq is None:
                    raise RuntimeError("request is missing routing correlation metadata")
                payload = measurement_payload(parts)
                if payload is None:
                    raise RuntimeError("request has an invalid measurement part layout")
                if not _reply_parts(received, parts, drain_timeout_s):
                    raise RuntimeError("reply remained backpressured during bounded drain")
                state["replied"] += 1
            finally:
                received.close()
    except BaseException as exc:
        state["error"] = exc


def _request_operation_sync(requester, routing_id, parts, timeout_s):
    operation = requester.request() if routing_id is None else requester.request(routing_id)
    return operation.messages(*parts).timeout(timeout_s).submit_sync(
        flags=zlink.SendFlags.NONE
    )


def _routing_probe(requester, routing_id, timeout_s):
    deadline = time.perf_counter() + timeout_s
    expected = measurement_parts(_PROBE_TOKEN)
    while time.perf_counter() < deadline:
        try:
            reply = _request_operation_sync(requester, routing_id, expected, timeout_s)
        except zlink.SubmitError as exc:
            if _transient_submit_result(exc.result):
                continue
            raise
        try:
            return tuple(part.to_bytes() for part in reply) == expected
        finally:
            _close_messages(reply)
    return False


def _run_requester(requester, routing_id, payload, *, run_id, msg_size, duration_s):
    timeout_s = max(0.001, resolve_single_reqrep_timeout_ms() / 1000.0)
    drain_timeout_s = max(0.001, resolve_single_reqrep_drain_timeout_ms() / 1000.0)
    latency = LatencySampler(resolve_single_latency_sample_cap())
    active_end = time.perf_counter() + duration_s
    outstanding = 0
    seq = 1
    completed = 0
    fatal = None

    def on_reply(parts, error):
        nonlocal outstanding, completed, fatal
        completed_at = time.perf_counter()
        outstanding -= 1
        if error is not None:
            if not (
                isinstance(error, zlink.RequestError)
                and error.result == zlink.RequestResult.TIMED_OUT
            ) and fatal is None:
                fatal = error
            return
        try:
            reply_bytes = tuple(part.to_bytes() for part in parts)
            data = measurement_payload(reply_bytes)
            header = None if data is None else decode_header(data)
            now_ns = time.time_ns()
            if (
                data is None
                or len(data) != msg_size
                or header is None
                or header["magic"] != HEADER_MAGIC
                or header["run_id"] != run_id
                or header["phase"] != 1
                or header["msg_size"] != msg_size
                or header["sent_ts_ns"] <= 0
                or now_ns < header["sent_ts_ns"]
                or completed_at >= active_end
            ):
                return
            completed += 1
            latency.add(float(now_ns - header["sent_ts_ns"]))
        finally:
            _close_messages(parts)

    with zlink.create_poller() as poller:
        poll_events = zlink.create_poll_events(1)
        poller.add_socket(requester, zlink.PollEventFlag.POLLCOMPLETION, 0)
        while time.perf_counter() < active_end and fatal is None:
            backpressured = False
            submitted_since_progress = 0
            while time.perf_counter() < active_end and fatal is None:
                stamped = stamp_payload(payload, phase=1, run_id=run_id, seq=seq)
                operation = (
                    requester.request()
                    if routing_id is None
                    else requester.request(routing_id)
                )
                outstanding += 1
                try:
                    operation.messages(*measurement_parts(stamped)).timeout(
                        timeout_s
                    ).submit_sync(
                        flags=zlink.SendFlags.DONT_WAIT,
                        callback=on_reply,
                    )
                except zlink.SubmitError as exc:
                    outstanding -= 1
                    if not _transient_submit_result(exc.result):
                        raise
                    backpressured = True
                    break
                seq += 1
                submitted_since_progress += 1
                if submitted_since_progress >= 64:
                    # Completion callbacks run when this requester-owned
                    # poller is progressed. Interleave a non-blocking poll
                    # with submission so an auto-HWM that remains writable
                    # for the whole active phase cannot defer every reply
                    # until after the active cutoff.
                    poller.wait(poll_events, 0)
                    submitted_since_progress = 0
            if outstanding and (backpressured or time.perf_counter() >= active_end):
                poller.wait(poll_events, 25)

        drain_end = time.perf_counter() + drain_timeout_s
        while outstanding and time.perf_counter() < drain_end:
            poller.wait(poll_events, 25)

    if outstanding:
        raise RuntimeError("request completion drain timed out")
    if fatal is not None:
        raise fatal
    if completed == 0 or latency.count == 0:
        raise RuntimeError("request-reply benchmark completed no active round trips")
    return result_metrics(
        count=completed,
        msg_size=msg_size,
        elapsed_s=duration_s,
        latency_sampler=latency,
        bandwidth_multiplier=2.0,
    )


def _send_stop(requester, routing_id):
    for _ in range(100):
        try:
            operation = requester.send() if routing_id is None else requester.send(routing_id)
            operation.message(STOP_TOKEN).submit_sync(flags=zlink.SendFlags.NONE)
            return
        except zlink.SubmitError as exc:
            if not _transient_submit_result(exc.result):
                raise
    raise RuntimeError("failed to submit request-reply stop token")


def run_reqrep_pattern(argv, *, pattern, routed_request):
    args = parse_single_args(argv, pattern=pattern.lower())
    run_id = benchmark_run_id()
    payload = new_payload(args.msg_size)
    request_timeout_s = max(0.001, resolve_single_reqrep_timeout_ms() / 1000.0)
    drain_timeout_s = max(0.001, resolve_single_reqrep_drain_timeout_ms() / 1000.0)

    with perf_context() as ctx:
        with zlink.create_router_socket(ctx) as replier:
            requester_factory = (
                zlink.create_router_socket if routed_request else zlink.create_dealer_socket
            )
            with requester_factory(ctx) as requester:
                replier.set_routing_id(b"SERVER")
                if routed_request:
                    requester.set_routing_id(b"CLIENT")
                    requester.router_options.connect_routing_id = b"SERVER"
                    requester.router_options.mandatory = True
                    replier.router_options.mandatory = True
                    routing_id = b"SERVER"
                else:
                    requester.set_routing_id(b"DEALER-REQ")
                    routing_id = None

                endpoint = resolve_single_endpoint(args.transport, pattern.lower())
                apply_single_socket_options(replier, requester)
                configure_single_tls_server(replier, args.transport)
                configure_single_tls_client(requester, args.transport)
                event = zlink.MonitorEventMask.CONNECTION_READY
                with replier.monitor_open(event) as replier_monitor:
                    with requester.monitor_open(event) as requester_monitor:
                        replier.bind(endpoint)
                        requester.connect(endpoint)
                        ready_timeout = resolve_single_connect_ready_timeout_ms()
                        wait_monitor_event(requester_monitor, event, timeout_ms=ready_timeout)
                        wait_monitor_event(replier_monitor, event, timeout_ms=ready_timeout)

                state = {"replied": 0, "error": None, "stop": False}
                replier_thread = threading.Thread(
                    target=_run_replier,
                    args=(replier, state, drain_timeout_s),
                    daemon=True,
                )
                replier_thread.start()
                metrics = None
                run_error = None
                try:
                    if not _routing_probe(requester, routing_id, request_timeout_s):
                        raise RuntimeError("request-reply routing probe failed")
                    metrics = _run_requester(
                        requester,
                        routing_id,
                        payload,
                        run_id=run_id,
                        msg_size=args.msg_size,
                        duration_s=args.duration,
                    )
                except BaseException as exc:
                    run_error = exc

                stop_error = None
                try:
                    _send_stop(requester, routing_id)
                except BaseException as exc:
                    state["stop"] = True
                    stop_error = exc
                replier_thread.join(timeout=drain_timeout_s)
                if replier_thread.is_alive():
                    raise RuntimeError("request-reply replier did not stop")
                if run_error is not None:
                    raise run_error
                if stop_error is not None:
                    raise stop_error
                if state["error"] is not None:
                    raise state["error"]
                if state["replied"] == 0:
                    raise RuntimeError("request-reply replier produced no replies")
                print_result_lines(pattern, args.transport, args.msg_size, metrics)
