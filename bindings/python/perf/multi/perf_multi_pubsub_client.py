import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    TOPIC,
    apply_multi_socket_options,
    active_message_latency_ns,
    benchmark_run_id,
    configure_multi_tls_client,
    LatencySampler,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    recv_nonblocking,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    measurement_part_count,
    safe_poll,
    wait_monitor_event,
)


def main(argv=None):
    args = parse_client_args(
        argv or sys.argv[1:], pattern="pubsub"
    )
    run_id = benchmark_run_id()
    latency_sampler = LatencySampler()
    count = 0

    with perf_client_context() as ctx:
        sockets = [zlink.create_sub_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for sock in sockets:
                    apply_multi_socket_options(sock)
                    monitor = stack.enter_context(
                        sock.monitor_open(
                            zlink.MonitorEventMask.CONNECTION_READY,
                            resolve_multi_monitor_hwm_bytes(),
                        )
                    )
                    configure_multi_tls_client(sock, args.transport)
                    sock.set_subscription(TOPIC)
                    sock.connect(args.endpoint)
                    monitors.append(monitor)
                for monitor in monitors:
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_multi_connect_ready_timeout_ms(),
                    )
            print(f"CLIENT_READY,{args.msg_size}", flush=True)
            while True:
                command = sys.stdin.readline().strip()
                if command == f"START,{args.msg_size}":
                    break
                raise SystemExit(f"unexpected command: {command}")

            active_deadline = time.perf_counter() + args.duration
            recv_storage = [zlink.create_topic_message() for _ in sockets]
            with zlink.create_poller() as poller:
                poll_events = zlink.create_poll_events(max(1, len(sockets)))
                for index, sock in enumerate(sockets):
                    poller.add_socket(sock, zlink.PollEventFlag.POLLIN, index)
                phase_done = False
                while not phase_done:
                    now = time.perf_counter()
                    if now >= active_deadline:
                        break
                    remaining_ms = max(1, int((active_deadline - now) * 1000))
                    ready_count = safe_poll(poller, poll_events, min(100, remaining_ms))
                    if not ready_count:
                        continue
                    for offset in range(ready_count):
                        index = poll_events.slot(offset)
                        if index < 0 or index >= len(sockets):
                            continue
                        current_sock = sockets[index]
                        while True:
                            if time.perf_counter() >= active_deadline:
                                phase_done = True
                                break
                            received = recv_nonblocking(
                                current_sock,
                                method="subscribe",
                                storage=recv_storage[index],
                            )
                            if received is None:
                                break
                            with received:
                                parts = received.parts
                                if not parts:
                                    continue
                                if len(parts) == 1 and parts[0].data == STOP_TOKEN:
                                    phase_done = True
                                    break
                                if len(parts) != measurement_part_count() or (
                                    len(parts) == 2 and len(parts[1].data) != 0
                                ):
                                    raise RuntimeError("invalid measured multipart pubsub payload")
                                data = parts[0].data
                                active, latency = active_message_latency_ns(
                                    data,
                                    expected_msg_size=args.msg_size,
                                    run_id=run_id,
                                )
                                if not active:
                                    continue
                                if time.perf_counter() >= active_deadline:
                                    continue
                                count += 1
                                if latency is not None:
                                    latency_sampler.add(latency)
                        if phase_done:
                            break

            if count <= 0:
                raise RuntimeError(
                    "multi pubsub benchmark did not receive any active message"
                )
            if latency_sampler.count == 0:
                raise RuntimeError(
                    "multi pubsub benchmark received active messages without latency samples"
                )
            metrics = result_metrics(
                count=count,
                msg_size=args.msg_size,
                elapsed_s=args.duration,
                latency_sampler=latency_sampler,
            )
            print_result_lines("MULTI_PUBSUB", args.transport, args.msg_size, metrics)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    main()
