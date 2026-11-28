import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    LatencySampler,
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

from perf_multi_measurement import (
    NS_PER_SECOND, PHASE_ACTIVE, SETTLE_SECONDS, Schedule,
    latency_duration, record_receive, wait_ms,
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

            async def run_phase(active_deadline, schedule=None, sampler=None):
                expected_part_count = measurement_part_count()
                completed = 0
                accepted = 0
                pending = set()
                failures = []
                deadline_ns = int(active_deadline * NS_PER_SECOND)
                sample_start = None if schedule is None else int((schedule.epoch + SETTLE_SECONDS) * NS_PER_SECOND)
                now = time.monotonic

                def observe_admission(due):
                    nonlocal accepted
                    when = now()
                    if schedule is not None:
                        schedule.admit(due, when)
                    elif when < active_deadline:
                        accepted += 1

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

                        observed = time.monotonic_ns()
                        if len(reply_parts) != expected_part_count:
                            return index, None
                        if expected_part_count == 2 and len(reply_parts[1].data) != 0:
                            return index, None
                        data = reply_parts[0].data
                        if len(data) != args.msg_size:
                            return index, None
                        if record_receive(
                            data, args.msg_size, run_id, observed, deadline_ns, sampler, 2,
                            sample_start=sample_start,
                        ):
                            completed += 1
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

                # G4 keeps submission stages separate, but the harness still owns
                # their progress. One completion-only public poller transfers both
                # admission and reply completions on this event-loop thread.
                with zlink.create_poller() as completion_poller:
                    completion_events = zlink.create_poll_events(
                        max(1, len(sockets))
                    )
                    registered_sockets = []
                    admissions = [None for _ in sockets]

                    def settle_admissions():
                        for index, admission in enumerate(admissions):
                            if admission is not None and admission[0].done():
                                terminal, due = admission
                                terminal.result()
                                admissions[index] = None
                                observe_admission(due)

                    try:
                        for index, sock in enumerate(sockets):
                            completion_poller.add_socket(
                                sock,
                                zlink.PollEventFlag.POLLCOMPLETION,
                                index,
                            )
                            registered_sockets.append(sock)

                        with scoped_relay_eager_task_factory():
                            next_sender = 0
                            while now() < active_deadline and not failures:
                                settle_admissions()
                                submitted_any = False
                                for _ in sockets:
                                    current = now()
                                    if current >= active_deadline or (
                                        schedule is not None and not schedule.due(current)
                                    ):
                                        break
                                    index = next_sender
                                    next_sender = (next_sender + 1) % len(sockets)
                                    sock = sockets[index]
                                    if admissions[index] is not None:
                                        continue
                                    stamped = bytes(
                                        stamp_payload(
                                            payloads[index],
                                            phase=PHASE_ACTIVE if schedule is None else schedule.phase(current),
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
                                    due = schedule.reserve() if schedule is not None else None
                                    submission = submit_managed_request(
                                        sock,
                                        stamped_parts,
                                        routing_id=(
                                            b"SERVER" if routed_request else None
                                        ),
                                        timeout_s=timeout_s,
                                    )
                                    task = asyncio.ensure_future(
                                        receive_reply(index, submission.reply)
                                    )
                                    pending.add(task)
                                    task.add_done_callback(observe_done)
                                    submitted_any = True
                                    if (
                                        submission.result
                                        == zlink.SubmitResult.BACKPRESSURED
                                    ):
                                        admissions[index] = submission.admitted, due
                                    else:
                                        observe_admission(due)

                                current = now()
                                until = active_deadline
                                if schedule is not None and not schedule.due(current):
                                    until = min(until, schedule.next())
                                completion_poller.wait(
                                    completion_events, 0 if submitted_any else wait_ms(current, until)
                                )
                                await asyncio.sleep(0)

                        drain_deadline = now() + drain_timeout_s
                        while (
                            (pending or any(admissions))
                            and now() < drain_deadline
                            and not failures
                        ):
                            settle_admissions()
                            if not pending and not any(admissions):
                                break
                            completion_poller.wait(completion_events, wait_ms(now(), drain_deadline))
                            await asyncio.sleep(0)
                        settle_admissions()

                        if pending or any(admissions):
                            still_pending = tuple(pending)
                            for task in still_pending:
                                task.cancel()
                            await asyncio.gather(
                                *still_pending, return_exceptions=True
                            )
                            if not failures:
                                raise RuntimeError(
                                    "request completion drain timed out"
                                )
                    finally:
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
                return completed, accepted

            completed, active_sends = await run_phase(time.monotonic() + args.duration)
            if completed == 0:
                raise RuntimeError(f"{pattern.lower()} benchmark completed no active replies")
            schedule = Schedule(time.monotonic(), completed, args.duration)
            latency_sampler = LatencySampler()
            paced, _ = await run_phase(
                schedule.epoch + SETTLE_SECONDS + latency_duration(args.duration),
                schedule, latency_sampler,
            )
            schedule.print(pattern, args, active_sends, completed, paced)
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
