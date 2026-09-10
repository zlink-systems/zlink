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
    print_multi_auto_hwm_detail,
    resolve_multi_connect_ready_timeout_ms,
    resolve_multi_reqrep_drain_timeout_ms,
    resolve_multi_reqrep_timeout_ms,
    result_metrics,
    scoped_relay_eager_task_factory,
    stamp_payload,
    wait_monitor_event,
)


def submit_managed_request(
    sock, payload_parts, *, routing_id=None, timeout_s
):
    """Submit once through the binding-owned WRITABLE retry machine."""

    operation = sock.request() if routing_id is None else sock.request(routing_id)
    return operation.messages(*payload_parts).timeout(timeout_s).submit()


def _wait_for_runner_stop_after_done():
    """Block until the runner sends STOP/QUIT after CLIENT_DONE.

    Mirrors bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:722-731.
    """

    for line in sys.stdin:
        if line.strip().upper() in {"STOP", "QUIT"}:
            return True
    return False


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

            async def receive_reply(index, reply):
                nonlocal completed
                reply_parts = None

                try:
                    try:
                        reply_parts = await reply
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

            async def submit_loop(index):
                while perf_counter() < active_deadline and not failures:
                    stamped = bytes(
                        stamp_payload(
                            payloads[index],
                            phase=1,
                            run_id=run_id,
                            seq=seqs[index],
                        )
                    )
                    seqs[index] += 1
                    stamped_parts = (
                        (stamped,)
                        if expected_part_count == 1
                        else (stamped, b"")
                    )
                    submission = submit_managed_request(
                        sockets[index],
                        stamped_parts,
                        routing_id=b"SERVER" if routed_request else None,
                        timeout_s=timeout_s,
                    )
                    task = asyncio.ensure_future(
                        receive_reply(index, submission.reply)
                    )
                    pending.add(task)
                    task.add_done_callback(observe_done)
                    if submission.result == zlink.SubmitResult.BACKPRESSURED:
                        await submission.admitted

            with scoped_relay_eager_task_factory():
                await asyncio.gather(
                    *(submit_loop(index) for index in range(len(sockets)))
                )

            if pending:
                still_pending = tuple(pending)
                try:
                    await asyncio.wait_for(
                        asyncio.gather(*still_pending), drain_timeout_s
                    )
                except asyncio.TimeoutError:
                    for task in still_pending:
                        task.cancel()
                    await asyncio.gather(*still_pending, return_exceptions=True)
                    raise RuntimeError("request completion drain timed out")

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
            if sockets:
                print_multi_auto_hwm_detail(
                    sockets[0],
                    "endpoint",
                    args.transport,
                    args.msg_size,
                    "router" if routed_request else "dealer",
                )
            print_result_lines(pattern, args.transport, args.msg_size, metrics)
            # PERF_MULTI_TEST_POLICY.md:379,386-388 / PERF_POLICY.md:483-486 -
            # emit CLIENT_DONE, keep the request completion target sockets
            # open, and close them only after the runner has stopped the
            # server and sent STOP. C reference:
            # bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:792,:722-731.
            print(f"CLIENT_DONE,{args.msg_size}", flush=True)
            if not _wait_for_runner_stop_after_done():
                raise RuntimeError(
                    "runner closed stdin before STOP after CLIENT_DONE"
                )
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)
