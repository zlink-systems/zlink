import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    LatencySampler,
    active_message_latency_ns,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    measurement_part_count,
    new_payload,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    resolve_multi_connect_ready_timeout_ms,
    resolve_multi_reqrep_drain_timeout_ms,
    resolve_multi_reqrep_timeout_ms,
    result_metrics,
    scoped_relay_eager_task_factory,
    stamp_payload,
    wait_monitor_event,
)


async def submit_managed_request(
    sock, payload_parts, *, routing_id=None, timeout_s
):
    """Submit once through the binding-owned WRITABLE retry machine."""

    operation = sock.request() if routing_id is None else sock.request(routing_id)
    return await operation.messages(*payload_parts).timeout(timeout_s).submit()


def _close_reply_parts(parts):
    if parts is None:
        return
    for part in parts:
        part.close()


async def run_reqrep_client(argv, *, pattern, routed_request):
    args = parse_client_args(argv, pattern=pattern.lower())
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    seqs = [1 for _ in range(args.clients)]
    latency_sampler = LatencySampler()
    completed = 0
    pending = set()
    failures = []
    timeout_s = max(0.001, resolve_multi_reqrep_timeout_ms() / 1000.0)
    drain_timeout_s = max(
        0.001, resolve_multi_reqrep_drain_timeout_ms() / 1000.0
    )

    with perf_client_context() as ctx:
        factory = zlink.create_router_socket if routed_request else zlink.create_dealer_socket
        sockets = [factory(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for index, sock in enumerate(sockets):
                    sock.set_routing_id(f"CLIENT-{index}".encode("ascii"))
                    if routed_request:
                        sock.router_options.connect_routing_id = b"SERVER"
                    monitor = stack.enter_context(
                        sock.monitor_open(
                            zlink.MonitorEventMask.CONNECTION_READY,
                            resolve_multi_monitor_hwm_bytes(),
                        )
                    )
                    configure_multi_tls_client(sock, args.transport)
                    apply_multi_socket_options(sock)
                    sock.connect(args.endpoint)
                    monitors.append(monitor)
                for monitor in monitors:
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_multi_connect_ready_timeout_ms(),
                    )

            # Match Node's connected-client policy: the live peer count owns
            # auto-HWM sizing, not the earlier socket-construction point.
            ctx.recalculate_auto_hwm()

            perf_counter = time.perf_counter
            active_deadline = perf_counter() + args.duration
            expected_part_count = measurement_part_count()

            async def request_once(index, stamped_parts):
                nonlocal completed
                reply_parts = None

                try:
                    try:
                        reply_parts = await submit_managed_request(
                            sockets[index],
                            stamped_parts,
                            routing_id=b"SERVER" if routed_request else None,
                            timeout_s=timeout_s,
                        )
                    except zlink.RequestError:
                        # Mirrors the C callback contract: a timed-out or
                        # otherwise terminal request is drained but is not an
                        # active completion or a process-fatal submit error.
                        return index, None

                    completed_at = perf_counter()
                    if completed_at >= active_deadline:
                        return index, None
                    if len(reply_parts) != expected_part_count:
                        return index, None
                    if expected_part_count == 2 and len(reply_parts[1].data) != 0:
                        return index, None
                    data = reply_parts[0].data
                    if len(data) != args.msg_size:
                        return index, None
                    active, latency = active_message_latency_ns(
                        data,
                        expected_msg_size=args.msg_size,
                        run_id=run_id,
                    )
                    if not active:
                        return index, None
                    completed += 1
                    if latency is not None:
                        latency_sampler.add(latency / 2.0)
                    return index, None
                finally:
                    _close_reply_parts(reply_parts)

            def observe_done(task):
                pending.discard(task)
                if task.cancelled():
                    return
                try:
                    task.result()
                except Exception as exc:
                    failures.append(exc)

            # Own request completion dispatch on this event-loop thread. If
            # Core dispatches each reply from its worker, the ctypes callback
            # must repeatedly contend for the GIL with this submit loop. A
            # completion poller keeps the public request API unchanged while
            # making callback and Future progression single-threaded.
            with zlink.create_poller() as completion_poller:
                completion_events = zlink.create_poll_events(
                    max(1, len(sockets))
                )
                registered_sockets = []
                try:
                    for index, sock in enumerate(sockets):
                        completion_poller.add_socket(
                            sock,
                            zlink.PollEventFlag.POLLCOMPLETION,
                            index,
                        )
                        registered_sockets.append(sock)

                    # Python 3.12 starts each request coroutine immediately,
                    # matching Node Promise construction and removing one
                    # scheduler turn plus the old nested Task. Keep each
                    # logical payload immutable until its managed request has
                    # either completed or retained its refusal-time snapshot.
                    with scoped_relay_eager_task_factory():
                        while perf_counter() < active_deadline and not failures:
                            for index in range(len(sockets)):
                                if perf_counter() >= active_deadline:
                                    break
                                stamped = stamp_payload(
                                    payloads[index],
                                    phase=1,
                                    run_id=run_id,
                                    seq=seqs[index],
                                )
                                seqs[index] += 1
                                stamped = bytes(stamped)
                                stamped_parts = (
                                    (stamped,)
                                    if expected_part_count == 1
                                    else (stamped, b"")
                                )
                                task = asyncio.create_task(
                                    request_once(index, stamped_parts)
                                )
                                if task.done():
                                    observe_done(task)
                                else:
                                    pending.add(task)
                                    task.add_done_callback(observe_done)
                            # Non-blocking progress avoids both a timer and a
                            # binding-owned inflight window. The following
                            # cooperative turn resumes the completed Futures.
                            completion_poller.wait(completion_events, 0)
                            await asyncio.sleep(0)

                    drain_deadline = perf_counter() + drain_timeout_s
                    while (
                        pending
                        and perf_counter() < drain_deadline
                        and not failures
                    ):
                        completion_poller.wait(completion_events, 0)
                        await asyncio.sleep(0)

                    if pending:
                        still_pending = tuple(pending)
                        for task in still_pending:
                            task.cancel()
                        await asyncio.gather(
                            *still_pending, return_exceptions=True
                        )
                        await asyncio.sleep(0)
                finally:
                    # A poller must release every registered requester before
                    # the outer socket cleanup; closing registered sockets can
                    # otherwise leave completion dispatch waiting on teardown.
                    for sock in registered_sockets:
                        try:
                            completion_poller.remove_socket(sock)
                        except Exception as exc:
                            print(
                                f"[perf] completion poller remove failed: {exc}",
                                file=sys.stderr,
                            )

            if failures:
                raise failures[0]
            if completed == 0:
                raise RuntimeError(
                    f"{pattern.lower()} benchmark completed no active replies"
                )
            if latency_sampler.count == 0:
                raise RuntimeError(
                    f"{pattern.lower()} benchmark completed without latency samples"
                )
            metrics = result_metrics(
                count=completed,
                msg_size=args.msg_size,
                elapsed_s=args.duration,
                latency_sampler=latency_sampler,
                bandwidth_multiplier=2.0,
            )
            print_result_lines(pattern, args.transport, args.msg_size, metrics)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)
