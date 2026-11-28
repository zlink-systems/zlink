import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    MultiSendTurnCoordinator,
    new_payload,
    parse_client_args,
    perf_client_context,
    print_multi_auto_hwm_detail,
    resolve_multi_monitor_hwm_bytes,
    resolve_multi_connect_ready_timeout_ms,
    resolve_multi_send_drain_timeout_ms,
    send_routed,
    stamp_payload,
    submit_routed,
    wait_monitor_event,
)

from perf_multi_measurement import (
    PHASE_ACTIVE, SETTLE_SECONDS, Schedule, latency_duration, read_phase_count,
)


async def _send_stop_token(sock, completion_poller, completion_events):
    await send_routed(
        sock,
        STOP_TOKEN,
        completion_poller=completion_poller,
        completion_events=completion_events,
        measurement=False,
    )


async def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="dealer_dealer")
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    seq = 0

    with perf_client_context() as ctx:
        sockets = [zlink.create_dealer_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for sock in sockets:
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
                # Readiness ownership ends at the barrier. Release these
                # monitors before the measured-path HWM snapshot opens one.
                stack.close()
                print(f"CLIENT_READY,{args.msg_size}", flush=True)
                command = sys.stdin.readline().strip()
                if command != f"START,{args.msg_size}":
                    raise SystemExit(f"unexpected command: {command}")

                ctx.recalculate_auto_hwm()
                with zlink.create_poller() as completion_poller:
                    completion_events = zlink.create_poll_events(
                        max(1, len(sockets))
                    )
                    for index, sock in enumerate(sockets):
                        completion_poller.add_socket(
                            sock,
                            zlink.PollEventFlag.POLLCOMPLETION,
                            index,
                        )

                    async def run_and_stop(deadline, schedule=None):
                        def submit(index):
                            nonlocal seq
                            seq += 1
                            phase = PHASE_ACTIVE if schedule is None else schedule.phase(time.monotonic())
                            return submit_routed(
                                sockets[index],
                                stamp_payload(payloads[index], phase=phase, run_id=run_id, seq=seq),
                            )

                        coordinator = MultiSendTurnCoordinator(
                            completion_poller, completion_events, len(sockets)
                        )
                        active_sends = await coordinator.run(
                            deadline, submit, schedule=schedule,
                            drain_timeout_ms=resolve_multi_send_drain_timeout_ms(),
                        )
                        for sock in sockets:
                            await _send_stop_token(sock, completion_poller, completion_events)
                        return active_sends

                    active_deadline = time.monotonic() + args.duration
                    active_sends = await run_and_stop(active_deadline)
                    received = read_phase_count("PHASE_PACED", args.msg_size)
                    schedule = Schedule(time.monotonic(), received, args.duration)
                    await run_and_stop(
                        schedule.epoch + SETTLE_SECONDS + latency_duration(args.duration), schedule
                    )
                    paced = read_phase_count("PHASE_COMPLETE", args.msg_size)
                    schedule.print("DEALER_DEALER", args, active_sends, received, paced)
                if sockets:
                    print_multi_auto_hwm_detail(
                        sockets[0], "endpoint", args.transport, args.msg_size, "dealer"
                    )
                # C run_single_size_case: send a wire stop token per socket
                # so the server receive window terminates.
                print(f"CLIENT_DONE,{args.msg_size}", flush=True)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
