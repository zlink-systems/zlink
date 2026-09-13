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

                active_deadline = time.perf_counter() + args.duration
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

                    def submit(index):
                        nonlocal seq
                        seq += 1
                        return submit_routed(
                            sockets[index],
                            stamp_payload(
                                payloads[index],
                                phase=1,
                                run_id=run_id,
                                seq=seq,
                            ),
                        )

                    coordinator = MultiSendTurnCoordinator(
                        completion_poller,
                        completion_events,
                        len(sockets),
                    )
                    await coordinator.run(
                        active_deadline,
                        submit,
                        drain_timeout_ms=resolve_multi_send_drain_timeout_ms(),
                    )
                    for current_sock in sockets:
                        await _send_stop_token(
                            current_sock,
                            completion_poller,
                            completion_events,
                        )
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
