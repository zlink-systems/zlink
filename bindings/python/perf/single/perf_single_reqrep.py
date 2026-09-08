import asyncio
import os
import sys
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

# Read once at import: PERF_POLICY.md § 8 diagnostic knob, never consulted on
# the measured path.
_DEBUG = os.environ.get("PERF_DEBUG") == "1"

# C parity (bindings/c/perf/single/common/perf_single_reqrep.hpp
# run_request_phase 396-443): drain completions without waiting every 64
# submissions so one long submission burst still settles replies as it goes.
_SUBMIT_PROGRESS_INTERVAL = 64

# C parity: the requester blocks bounded on the completion poller only when the
# admission window is full, so a saturated interval cannot spin.
_SATURATED_PROGRESS_WAIT_MS = 50


def _close_messages(parts):
    for part in parts:
        part.close()


def _reply_parts(received, parts):
    received.reply().messages(*parts).submit()


def _run_replier(replier, state):
    received = zlink.create_received()
    try:
        with zlink.create_poller() as poller:
            events = zlink.create_poll_events(1)
            poller.add_socket(replier, zlink.PollEventFlag.POLLIN, 0)
            try:
                while not state["stop"]:
                    if not poller.wait(events, resolve_single_reqrep_timeout_ms()):
                        continue
                    while replier.recv_into(received, flags=zlink.RecvFlags.DONT_WAIT):
                        try:
                            parts = tuple(received)
                            if len(parts) == 1 and parts[0].to_bytes() == STOP_TOKEN:
                                return
                            if received.routing_id is None or received.reply_token is None:
                                raise RuntimeError("request is missing routing correlation metadata")
                            if measurement_payload(parts) is None:
                                raise RuntimeError("request has an invalid measurement part layout")
                            _reply_parts(received, parts)
                            state["replied"] += 1
                        finally:
                            received.close()
            finally:
                poller.remove_socket(replier)
    except BaseException as exc:
        state["error"] = exc


def _request_operation_sync(requester, routing_id, parts, timeout_s):
    operation = requester.request() if routing_id is None else requester.request(routing_id)
    return operation.messages(*parts).timeout(timeout_s).submit_sync()


def _routing_probe(requester, routing_id, timeout_s):
    expected = measurement_parts(_PROBE_TOKEN)
    reply = _request_operation_sync(requester, routing_id, expected, timeout_s)
    try:
        return tuple(part.to_bytes() for part in reply) == expected
    finally:
        _close_messages(reply)


async def _request_operation(requester, routing_id, parts, timeout_s):
    operation = requester.request() if routing_id is None else requester.request(routing_id)
    return await operation.messages(*parts).timeout(timeout_s).submit()


def _applied_send_hwm_bytes(monitor):
    """Applied SNDHWM bytes from the socket's own auto-HWM snapshot."""

    status = monitor.status()
    return int(getattr(status, "auto_hwm_applied_sndhwm_bytes", 0) or 0)


def _admission_window_requests(applied_sndhwm_bytes, socket_send_hwm_bytes, wire_size):
    """PERF_SINGLE_TEST_POLICY.md § 1.1.3 (D-BP40).

    The public request terminal is one awaitable over admission and reply, so
    the runner never sees the admission boundary itself. It reproduces the C
    reference boundary instead: the un-settled request set is bounded by the
    admission window Core applied to this socket - the applied SNDHWM bytes
    divided by one request's wire size (header included) - which is exactly the
    window whose exhaustion makes the C runner see ZLINK_SUBMIT_BACKPRESSURED
    (bindings/c/perf/single/common/perf_single_reqrep.hpp run_request_phase).
    It is not a fixed number and no runner-side cap is added on top of it.
    A manual PERF_SINGLE_SNDHWM override never reaches the auto-HWM snapshot, so
    the socket option is the only fallback; with neither value the window is
    unknown and the benchmark fails instead of inventing one.
    """

    hwm_bytes = int(applied_sndhwm_bytes or 0)
    if hwm_bytes <= 0:
        hwm_bytes = int(socket_send_hwm_bytes or 0)
    if hwm_bytes <= 0:
        raise RuntimeError(
            "requester socket reports no send high-water mark, "
            "so the admission window is unknown"
        )
    window = hwm_bytes // max(1, int(wire_size))
    return window if window > 0 else 1


async def _run_requester_async(
    requester, routing_id, payload, *, run_id, msg_size, duration_s, admission_window
):
    timeout_s = max(0.001, resolve_single_reqrep_timeout_ms() / 1000.0)
    drain_timeout_s = max(
        0.001, resolve_single_reqrep_drain_timeout_ms() / 1000.0
    )
    latency = LatencySampler(resolve_single_latency_sample_cap())
    active_end = time.perf_counter() + duration_s
    seq = 1
    completed = 0
    pending = set()
    failures = []
    expected_part_count = len(measurement_parts(b""))
    async def request_once(stamped_parts):
        nonlocal completed
        parts = None
        try:
            try:
                parts = await _request_operation(
                    requester, routing_id, stamped_parts, timeout_s
                )
            except zlink.RequestError as exc:
                if exc.result == zlink.RequestResult.TIMED_OUT:
                    return
                raise
            completed_at = time.perf_counter()
            reply_bytes = tuple(part.to_bytes() for part in parts)
            data = measurement_payload(reply_bytes)
            header = None if data is None else decode_header(data)
            now_ns = time.monotonic_ns()
            if (
                data is not None
                and len(data) == msg_size
                and header is not None
                and header["magic"] == HEADER_MAGIC
                and header["run_id"] == run_id
                and header["phase"] == 1
                and header["msg_size"] == msg_size
                and header["sent_ts_ns"] > 0
                and now_ns >= header["sent_ts_ns"]
                and completed_at < active_end
            ):
                completed += 1
                # PERF_SINGLE_TEST_POLICY.md § 1.1: request-reply latency is the
                # round trip from request submission to reply completion, the
                # same quantity C records (perf_single_reqrep.hpp
                # record_request_completion 192-198). It is not halved.
                latency.add(float(now_ns - header["sent_ts_ns"]))
        finally:
            if parts is not None:
                _close_messages(parts)

    def observe_done(task):
        pending.discard(task)
        if task.cancelled():
            return
        try:
            task.result()
        except BaseException as exc:
            failures.append(exc)

    with zlink.create_poller() as completion_poller:
        completion_events = zlink.create_poll_events(1)
        completion_poller.add_socket(
            requester,
            zlink.PollEventFlag.POLLCOMPLETION,
            0,
        )
        try:
            # A turn submits until the un-settled set fills the admission window,
            # then progresses completions on this same thread. Nothing here waits
            # for a reply before submitting the next request, and the window -
            # not a runner constant - is what stops the submit loop
            # (§ 1.1.3, D-BP40; C run_request_phase 396-443).
            while time.perf_counter() < active_end and not failures:
                submitted_any = False
                submitted_since_progress = 0
                while (
                    time.perf_counter() < active_end
                    and not failures
                    and len(pending) < admission_window
                ):
                    stamped = bytes(
                        stamp_payload(payload, phase=1, run_id=run_id, seq=seq)
                    )
                    seq += 1
                    stamped_parts = (
                        (stamped,) if expected_part_count == 1 else (stamped, b"")
                    )
                    task = asyncio.create_task(request_once(stamped_parts))
                    pending.add(task)
                    task.add_done_callback(observe_done)
                    submitted_any = True
                    submitted_since_progress += 1
                    if submitted_since_progress >= _SUBMIT_PROGRESS_INTERVAL:
                        submitted_since_progress = 0
                        completion_poller.wait(completion_events, 0)
                        await asyncio.sleep(0)
                # Blocking bounded progress is reached only when the window is
                # full (or the deadline passed), matching C's poll(50) after
                # backpressure. This thread owns the completion drain, so the
                # wait is this runner progressing its own requests.
                completion_poller.wait(
                    completion_events,
                    0 if submitted_any else _SATURATED_PROGRESS_WAIT_MS,
                )
                await asyncio.sleep(0)

            # Bounded completion drain of requests submitted before the deadline;
            # every one of them is bounded by its own reply timeout and no new
            # request is submitted here.
            drain_deadline = time.perf_counter() + drain_timeout_s
            drain_wait_ms = 0
            while pending and time.perf_counter() < drain_deadline and not failures:
                outstanding = len(pending)
                completion_poller.wait(completion_events, drain_wait_ms)
                await asyncio.sleep(0)
                drain_wait_ms = (
                    0
                    if len(pending) != outstanding
                    else _SATURATED_PROGRESS_WAIT_MS
                )
            if pending:
                still_pending = tuple(pending)
                for task in still_pending:
                    task.cancel()
                await asyncio.gather(*still_pending, return_exceptions=True)
                raise RuntimeError("request completion drain timed out")
        finally:
            completion_poller.remove_socket(requester)

    if failures:
        raise failures[0]

    if completed == 0 or latency.count == 0:
        raise RuntimeError("request-reply benchmark completed no active round trips")
    return result_metrics(
        count=completed,
        msg_size=msg_size,
        elapsed_s=duration_s,
        latency_sampler=latency,
        bandwidth_multiplier=2.0,
    )


def _run_requester_thread(requester, routing_id, payload, options, state):
    """Drive the requester on its own OS thread and its own private loop.

    PERF_SINGLE_TEST_POLICY.md § 1.1.5 judges the progress driver, not the
    awaitable type. The public Python request terminal is
    `RequestOp.submit()`, an `async def` that calls `asyncio.get_running_loop()`
    before it touches the socket (bindings/python/src/zlink/_runtime/messaging/
    routed_async.py submit_request), so the awaitable cannot be polled by a
    plain thread: driving the coroutine without a running loop raises
    `RuntimeError: no running event loop`. The only other public terminal,
    `submit_sync()`, blocks until the reply and would pin in-flight to 1.
    This loop is therefore created by the runner, lives only inside this
    dedicated thread, and is stepped only by it - the requester submits,
    drains its own completions through its own POLLCOMPLETION poller, and
    yields exactly one turn of its own loop to settle them. No shared pool,
    executor, or other thread's loop participates.
    """

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    eager_factory = getattr(asyncio, "eager_task_factory", None)
    if eager_factory is not None:
        loop.set_task_factory(eager_factory)
    try:
        state["metrics"] = loop.run_until_complete(
            _run_requester_async(requester, routing_id, payload, **options)
        )
    except BaseException as exc:
        state["error"] = exc
    finally:
        loop.set_task_factory(None)
        asyncio.set_event_loop(None)
        loop.close()


def _send_stop(requester, routing_id):
    operation = requester.send() if routing_id is None else requester.send(routing_id)
    operation.message(STOP_TOKEN).submit_sync()


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
                        # Read the applied admission window while the requester
                        # monitor is still open (§ 1.1.3, D-BP40).
                        admission_window = _admission_window_requests(
                            _applied_send_hwm_bytes(requester_monitor),
                            requester.options.send_high_water_mark,
                            len(payload),
                        )
                if _DEBUG:
                    print(
                        "single_reqrep_debug:"
                        f"admission_window={admission_window}:"
                        f"wire_size={len(payload)}",
                        file=sys.stderr,
                    )

                state = {"replied": 0, "error": None, "stop": False}
                replier_thread = threading.Thread(
                    target=_run_replier,
                    args=(replier, state),
                    daemon=True,
                )
                replier_thread.start()
                metrics = None
                run_error = None
                try:
                    if not _routing_probe(requester, routing_id, request_timeout_s):
                        raise RuntimeError("request-reply routing probe failed")
                    requester_state = {"metrics": None, "error": None}
                    requester_thread = threading.Thread(
                        target=_run_requester_thread,
                        args=(
                            requester,
                            routing_id,
                            payload,
                            {
                                "run_id": run_id,
                                "msg_size": args.msg_size,
                                "duration_s": args.duration,
                                "admission_window": admission_window,
                            },
                            requester_state,
                        ),
                    )
                    requester_thread.start()
                    requester_thread.join(
                        timeout=args.duration + drain_timeout_s + 5.0
                    )
                    if requester_thread.is_alive():
                        raise RuntimeError("requester thread did not finish")
                    if requester_state["error"] is not None:
                        raise requester_state["error"]
                    metrics = requester_state["metrics"]
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
