import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    apply_multi_auto_hwm_msg_unit,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    is_active_message,
    latency_ns_from_message,
    new_payload,
    parse_client_args,
    perf_client_context,
    print_result_lines,
    received_metric_payload,
    recv_nonblocking,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    safe_poll,
    send_nonblocking,
    stamp_payload,
    wait_monitor_event,
)


def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="router_router")
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    # C run_echo_window_round_robin: purely deadline-driven, no stop token.
    waiting_reply = [False] * args.clients
    send_pending = [True] * args.clients
    received = 0
    latencies = []
    seq = 0

    with perf_client_context() as ctx:
        apply_multi_auto_hwm_msg_unit(ctx, args.msg_size)
        sockets = [zlink.create_router_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for index, sock in enumerate(sockets):
                    sock.set_routing_id(f"CLIENT-{index}".encode("ascii"))
                    sock.router_options.connect_routing_id = b"SERVER"
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
                        poller.add_socket(
                            sock,
                            zlink.PollEventFlag.POLLIN | zlink.PollEventFlag.POLLOUT,
                            index,
                        )
                    # C run_echo_window_round_robin: deadline-driven (no stop
                    # token); -1 signal-driven poll dispatching only ready
                    # sources.
                    while time.perf_counter() < active_deadline:
                        for index, current_sock in enumerate(sockets):
                            if waiting_reply[index] or not send_pending[index]:
                                continue
                            seq += 1
                            payload = stamp_payload(
                                payloads[index],
                                phase=1,
                                run_id=run_id,
                                seq=seq,
                            )
                            if send_nonblocking(
                                current_sock,
                                payload,
                                routing_id=b"SERVER",
                            ):
                                waiting_reply[index] = True
                                send_pending[index] = False

                        interest = [
                            i
                            for i in range(len(sockets))
                            if waiting_reply[i] or send_pending[i]
                        ]
                        if not interest:
                            for i in range(len(sockets)):
                                if not waiting_reply[i]:
                                    send_pending[i] = True
                            continue

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
                                seq += 1
                                payload = stamp_payload(
                                    payloads[index],
                                    phase=1,
                                    run_id=run_id,
                                    seq=seq,
                                )
                                if send_nonblocking(
                                    current_sock,
                                    payload,
                                    routing_id=b"SERVER",
                                ):
                                    waiting_reply[index] = True
                                    send_pending[index] = False
                            if not (ev & int(zlink.PollEventFlag.POLLIN)):
                                continue
                            while True:
                                msg = recv_nonblocking(
                                    current_sock,
                                    storage=recv_storage[index],
                                )
                                if msg is None:
                                    break
                                with msg:
                                    data = received_metric_payload(msg)
                                    lat = (
                                        latency_ns_from_message(data)
                                        if data
                                        else None
                                    )
                                    # C: every matched header counts; latency
                                    # excludes clock-skew, halved for round trip.
                                    if data and is_active_message(
                                        data,
                                        expected_msg_size=args.msg_size,
                                        run_id=run_id,
                                    ):
                                        received += 1
                                        if lat is not None:
                                            latencies.append(lat / 2.0)
                                waiting_reply[index] = False
                                if time.perf_counter() < active_deadline:
                                    send_pending[index] = True
                if received == 0:
                    raise RuntimeError(
                        "multi router-router benchmark did not receive any active reply"
                    )
                metrics = result_metrics(
                    count=received,
                    msg_size=args.msg_size,
                    elapsed_s=args.duration,
                    latencies_ns=latencies,
                    bandwidth_multiplier=2.0,
                )
                print_result_lines(
                    "MULTI_ROUTER_ROUTER", args.transport, args.msg_size, metrics
                )
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    main()
