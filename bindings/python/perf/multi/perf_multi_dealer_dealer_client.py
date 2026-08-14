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
    new_payload,
    parse_client_args,
    perf_client_context,
    resolve_multi_connect_ready_timeout_ms,
    safe_poll,
    send_routed,
    stamp_payload,
    wait_monitor_event,
)


async def _send_stop_token(sock):
    # C perf_multi_dealer_dealer_client.cpp send_stop_token: bounded retry
    # through transient backpressure so the server's receive window observes
    # the per-socket wire stop token.
    for _ in range(1000):
        if await send_routed(sock, STOP_TOKEN):
            return True
    return False


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
                        sock.monitor_open(zlink.MonitorEventMask.CONNECTION_READY)
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
                print(f"CLIENT_READY,{args.msg_size}", flush=True)
                command = sys.stdin.readline().strip()
                if command != f"START,{args.msg_size}":
                    raise SystemExit(f"unexpected command: {command}")

                active_deadline = time.perf_counter() + args.duration
                send_pending = [False] * len(sockets)
                with zlink.create_poller() as poller:
                    poll_events = zlink.create_poll_events(max(1, len(sockets)))
                    for index, sock in enumerate(sockets):
                        poller.add_socket(sock, zlink.PollEventFlag.POLLOUT, index)
                    # Await routed admission per socket. Retain the POLLOUT
                    # fallback only for a transient pre-admission rejection.
                    while time.perf_counter() < active_deadline:
                        for index, current_sock in enumerate(sockets):
                            if send_pending[index]:
                                continue
                            while time.perf_counter() < active_deadline:
                                seq += 1
                                if await send_routed(
                                    current_sock,
                                    stamp_payload(
                                        payloads[index],
                                        phase=1,
                                        run_id=run_id,
                                        seq=seq,
                                    ),
                                ):
                                    continue
                                # EAGAIN/backpressure: defer until POLLOUT.
                                send_pending[index] = True
                                break
                        if time.perf_counter() >= active_deadline:
                            break
                        if not any(send_pending):
                            continue
                        remaining_ms = int(
                            (active_deadline - time.perf_counter()) * 1000
                        )
                        if remaining_ms <= 0:
                            break
                        ready_count = safe_poll(
                            poller, poll_events, max(1, remaining_ms)
                        )
                        if not ready_count:
                            continue
                        for offset in range(ready_count):
                            if not (
                                poll_events.revents(offset)
                                & int(zlink.PollEventFlag.POLLOUT)
                            ):
                                continue
                            idx = poll_events.slot(offset)
                            if idx < 0 or idx >= len(sockets):
                                continue
                            send_pending[idx] = False
                # C run_single_size_case: send a wire stop token per socket
                # so the server receive window terminates.
                for sock in sockets:
                    await _send_stop_token(sock)
                print(f"CLIENT_DONE,{args.msg_size}", flush=True)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
