import sys
import time
import os
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    TOPIC,
    apply_multi_auto_hwm_msg_unit,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    latency_ns_from_message,
    parse_client_args,
    perf_client_context,
    print_result_lines,
    recv_nonblocking,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    safe_poll,
    wait_monitor_event,
)


def _positive_int_env(name, default):
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    try:
        parsed = int(value)
    except ValueError:
        return default
    return parsed if parsed > 0 else default


def _should_sample_latency(index, stride):
    return stride <= 1 or index == 1 or index % stride == 0


def main(argv=None):
    args = parse_client_args(
        argv or sys.argv[1:], pattern="pubsub"
    )
    run_id = benchmark_run_id()
    latencies = []
    count = 0
    latency_stride = _positive_int_env("PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE", 32)

    with perf_client_context() as ctx:
        apply_multi_auto_hwm_msg_unit(ctx, args.msg_size)
        sockets = [zlink.create_sub_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for sock in sockets:
                    apply_multi_socket_options(sock)
                    monitor = stack.enter_context(
                        sock.monitor_open(zlink.MonitorEventMask.CONNECTION_READY)
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
            stopped = [False] * len(sockets)
            recv_storage = [zlink.create_topic_message() for _ in sockets]
            with zlink.create_poller() as poller:
                poll_events = zlink.create_poll_events(max(1, len(sockets)))
                for index, sock in enumerate(sockets):
                    poller.add_socket(sock, zlink.PollEventFlag.POLLIN, index)
                stop_wait_deadline = active_deadline + 6.0
                # PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait. Each
                # subscriber exits on the wire-level stop token. If PubSub
                # drops that final token for a subset of subscribers, finish
                # after the server cooldown window once active traffic was
                # already observed.
                while not all(stopped):
                    now = time.perf_counter()
                    if count > 0 and now >= stop_wait_deadline:
                        break
                    remaining_ms = max(1, int((stop_wait_deadline - now) * 1000))
                    ready_count = safe_poll(poller, poll_events, min(1000, remaining_ms))
                    if not ready_count:
                        continue
                    for offset in range(ready_count):
                        index = poll_events.slot(offset)
                        if index < 0 or index >= len(sockets):
                            continue
                        current_sock = sockets[index]
                        if stopped[index]:
                            continue
                        while True:
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
                                data = parts[-1].data
                                if len(data) == len(STOP_TOKEN) and data == STOP_TOKEN:
                                    stopped[index] = True
                                    break
                                if len(data) != args.msg_size:
                                    continue
                                if time.perf_counter() >= active_deadline:
                                    continue
                                count += 1
                                if _should_sample_latency(count, latency_stride):
                                    latency = latency_ns_from_message(data)
                                    if latency is not None:
                                        latencies.append(latency)

            if count <= 0:
                raise RuntimeError(
                    "multi pubsub benchmark did not receive any active message"
                )
            metrics = result_metrics(
                count=count,
                msg_size=args.msg_size,
                elapsed_s=args.duration,
                latencies_ns=latencies,
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
