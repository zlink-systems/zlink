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
    measurement_parts,
    metric_payload_data,
    new_payload,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    resolve_multi_connect_ready_timeout_ms,
    resolve_multi_reqrep_drain_timeout_ms,
    resolve_multi_reqrep_timeout_ms,
    result_metrics,
    stamp_payload,
    wait_monitor_event,
)


async def request_with_admission_retry(
    sock,
    payload_parts,
    *,
    routing_id=None,
    timeout_s,
    on_admitted=None,
):
    """Await one public request, rebuilding only after pre-admission EAGAIN."""

    while True:
        try:
            operation = (
                sock.request()
                if routing_id is None
                else sock.request(routing_id)
            )
            attempt = asyncio.create_task(
                operation.messages(*payload_parts).timeout(timeout_s).submit()
            )
            # The public request coroutine performs admission before its first
            # suspension. Pending after one turn therefore means Core accepted
            # it and it is awaiting a reply; a completed task carries either
            # an inline reply or an immediate admission error.
            await asyncio.sleep(0)
            if not attempt.done() and on_admitted is not None:
                on_admitted()
                on_admitted = None
            try:
                result = await attempt
            except zlink.SubmitError as exc:
                if exc.result != zlink.SubmitResult.BACKPRESSURED:
                    if on_admitted is not None:
                        on_admitted()
                    raise
                # The request was not accepted. Preserve the same logical
                # payload and retry after one cooperative turn.
                await asyncio.sleep(0)
                continue
            if on_admitted is not None:
                on_admitted()
            return result
        except zlink.SubmitError as exc:
            if exc.result != zlink.SubmitResult.BACKPRESSURED:
                if on_admitted is not None:
                    on_admitted()
                raise
            # The request was not accepted. Preserve the same logical payload
            # and retry after one cooperative turn, without a timer or queue.
            await asyncio.sleep(0)


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
    admission_in_progress = [False for _ in range(args.clients)]
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

            active_deadline = time.perf_counter() + args.duration

            async def request_once(index, stamped_parts):
                nonlocal completed
                reply_parts = None
                admission_released = False

                def on_admitted():
                    nonlocal admission_released
                    admission_released = True
                    admission_in_progress[index] = False

                try:
                    try:
                        reply_parts = await request_with_admission_retry(
                            sockets[index],
                            stamped_parts,
                            routing_id=b"SERVER" if routed_request else None,
                            timeout_s=timeout_s,
                            on_admitted=on_admitted,
                        )
                    except zlink.RequestError:
                        # Mirrors the C callback contract: a timed-out or
                        # otherwise terminal request is drained but is not an
                        # active completion or a process-fatal submit error.
                        return

                    completed_at = time.perf_counter()
                    if completed_at >= active_deadline:
                        return
                    if len(reply_parts) != len(stamped_parts):
                        return
                    if len(reply_parts) == 2 and len(reply_parts[1].data) != 0:
                        return
                    data = metric_payload_data(
                        reply_parts[0].data,
                        expected_size=args.msg_size,
                    )
                    if not data:
                        return
                    active, latency = active_message_latency_ns(
                        data,
                        expected_msg_size=args.msg_size,
                        run_id=run_id,
                    )
                    if not active:
                        return
                    completed += 1
                    if latency is not None:
                        latency_sampler.add(latency / 2.0)
                finally:
                    if not admission_released:
                        admission_in_progress[index] = False
                    _close_reply_parts(reply_parts)

            def observe_done(task):
                pending.discard(task)
                if task.cancelled():
                    return
                exc = task.exception()
                if exc is not None:
                    failures.append(exc)

            while time.perf_counter() < active_deadline and not failures:
                for index in range(len(sockets)):
                    if time.perf_counter() >= active_deadline:
                        break
                    if admission_in_progress[index]:
                        continue
                    stamped = bytes(
                        stamp_payload(
                            payloads[index],
                            phase=1,
                            run_id=run_id,
                            seq=seqs[index],
                        )
                    )
                    seqs[index] += 1
                    admission_in_progress[index] = True
                    task = asyncio.create_task(
                        request_once(index, measurement_parts(stamped))
                    )
                    pending.add(task)
                    task.add_done_callback(observe_done)
                # Start every newly-created public request and let reply
                # completions run. No application window limits accepted
                # depth; Core/HWM owns admission.
                await asyncio.sleep(0)

            if pending:
                _, still_pending = await asyncio.wait(
                    tuple(pending), timeout=drain_timeout_s
                )
                if still_pending:
                    for task in still_pending:
                        task.cancel()
                    await asyncio.gather(*still_pending, return_exceptions=True)
                await asyncio.sleep(0)

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
