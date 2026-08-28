import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    active_message_latency_ns,
    benchmark_run_id,
    configure_multi_tls_client,
    LatencySampler,
    new_payload,
    parse_client_args,
    perf_client_context,
    print_result_lines,
    recv_nonblocking,
    received_metric_payload,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    safe_poll,
    send_routed,
    stamp_payload,
    wait_monitor_event,
)


async def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="dealer_router")
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    # C perf_multi_client_helpers.hpp run_echo_window_round_robin: the echo
    # client is purely deadline-driven (no wire stop token); awaiting_reply /
    # send_pending gate inflight==1 per socket.
    waiting_reply = [False] * args.clients
    send_pending = [True] * args.clients
    received = 0
    latency_sampler = LatencySampler()
    seq = 0

    with perf_client_context() as ctx:
        sockets = [zlink.create_dealer_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for index, sock in enumerate(sockets):
                    sock.set_routing_id(f"CLIENT-{index}".encode("ascii"))
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

                active_deadline = time.perf_counter() + args.duration
                recv_storage = [zlink.create_received() for _ in sockets]
                with zlink.create_poller() as poller:
                    poll_events = zlink.create_poll_events(max(1, len(sockets)))
                    for index, sock in enumerate(sockets):
                        poller.add_socket(sock, zlink.PollEventFlag.POLLIN, index)

                    # Attempt one initial request per socket. A blocked send
                    # changes only that socket's interest to POLLOUT; a
                    # successful send waits only for its reply on POLLIN.
                    for index, current_sock in enumerate(sockets):
                        next_seq = seq + 1
                        payload = stamp_payload(
                            payloads[index],
                            phase=1,
                            run_id=run_id,
                            seq=next_seq,
                        )
                        if await send_routed(current_sock, payload):
                            seq = next_seq
                            waiting_reply[index] = True
                            send_pending[index] = False
                        else:
                            poller.modify_socket(
                                current_sock, zlink.PollEventFlag.POLLOUT
                            )

                    # Dispatch only ready sockets. State transitions change
                    # poll interest, so reply wait does not receive writable
                    # wakeups and blocked sends resume only on POLLOUT.
                    while time.perf_counter() < active_deadline:
                        remaining_ms = int((active_deadline - time.perf_counter()) * 1000)
                        if remaining_ms <= 0:
                            break
                        ready_count = safe_poll(poller, poll_events, max(1, remaining_ms))
                        if not ready_count:
                            continue
                        for offset in range(ready_count):
                            index = poll_events.slot(offset)
                            if index < 0 or index >= len(sockets):
                                continue
                            current_sock = sockets[index]
                            ev = poll_events.revents(offset)
                            if (
                                ev & int(zlink.PollEventFlag.POLLOUT)
                                and not waiting_reply[index]
                                and send_pending[index]
                            ):
                                next_seq = seq + 1
                                payload = stamp_payload(
                                    payloads[index],
                                    phase=1,
                                    run_id=run_id,
                                    seq=next_seq,
                                )
                                if await send_routed(current_sock, payload):
                                    seq = next_seq
                                    waiting_reply[index] = True
                                    send_pending[index] = False
                                    poller.modify_socket(
                                        current_sock, zlink.PollEventFlag.POLLIN
                                    )
                            if not (
                                ev & int(zlink.PollEventFlag.POLLIN)
                                and waiting_reply[index]
                            ):
                                continue
                            while True:
                                msg = recv_nonblocking(
                                    current_sock,
                                    storage=recv_storage[index],
                                )
                                if msg is None:
                                    break
                                with msg:
                                    active = False
                                    latency = None
                                    data = received_metric_payload(
                                        msg, expected_size=args.msg_size
                                    )
                                    if data:
                                        active, latency = active_message_latency_ns(
                                            data, expected_msg_size=args.msg_size,
                                            run_id=run_id,
                                        )
                                    # C: every matched header counts (++local_recv,
                                    # -> recv_count); latency only added when not
                                    # clock-skewed (sent_ts_ns>0 && now>=sent_ts),
                                    # halved for the round trip.
                                    if active:
                                        received += 1
                                        if latency is not None:
                                            latency_sampler.add(latency / 2.0)
                                waiting_reply[index] = False
                                if time.perf_counter() < active_deadline:
                                    send_pending[index] = True
                                    next_seq = seq + 1
                                    payload = stamp_payload(
                                        payloads[index],
                                        phase=1,
                                        run_id=run_id,
                                        seq=next_seq,
                                    )
                                    if await send_routed(current_sock, payload):
                                        seq = next_seq
                                        waiting_reply[index] = True
                                        send_pending[index] = False
                                    else:
                                        poller.modify_socket(
                                            current_sock,
                                            zlink.PollEventFlag.POLLOUT,
                                        )
                                if waiting_reply[index]:
                                    break
                if received == 0:
                    raise RuntimeError(
                        "multi dealer-router benchmark did not receive any active reply"
                    )
                metrics = result_metrics(
                    count=received,
                    msg_size=args.msg_size,
                    elapsed_s=args.duration,
                    latency_sampler=latency_sampler,
                    bandwidth_multiplier=2.0,
                )
                print_result_lines(
                    "MULTI_DEALER_ROUTER_SENDSEND",
                    args.transport,
                    args.msg_size,
                    metrics,
                )
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
