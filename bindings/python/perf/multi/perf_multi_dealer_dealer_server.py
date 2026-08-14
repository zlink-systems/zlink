import sys
import threading
import time

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    active_message_latency_ns,
    apply_multi_socket_options,
    benchmark_endpoint,
    benchmark_run_id,
    configure_multi_tls_server,
    LatencySampler,
    print_result_lines,
    parse_server_args,
    perf_server_context,
    result_metrics,
    safe_poll,
)


def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    run_id = benchmark_run_id()
    endpoint = benchmark_endpoint(args.transport, "multi-dealer-dealer")
    active_duration_s = max(1.0, float(args.duration))
    start_event = threading.Event()
    stop_event = threading.Event()

    def read_commands():
        for line in sys.stdin:
            text = line.strip().upper()
            if text == f"START,{args.msg_size}":
                start_event.set()
            elif text in {"STOP", "QUIT"}:
                stop_event.set()
                start_event.set()
                return

    threading.Thread(target=read_commands, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_dealer_socket(ctx) as dealer:
            configure_multi_tls_server(dealer, args.transport)
            apply_multi_socket_options(dealer)
            with dealer.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as monitor:
                dealer.bind(endpoint)
                print(f"READY,{endpoint}", flush=True)
                with zlink.create_poller() as poller:
                    poller.add_socket(dealer, zlink.PollEventFlag.POLLIN, 0)
                    poll_events = zlink.create_poll_events(1)
                    start_event.wait()
                    if stop_event.is_set():
                        return
                    active_deadline = time.perf_counter() + active_duration_s
                    latency_sampler = LatencySampler()
                    count = 0
                    recv_storage = zlink.create_received()
                    recv_into = dealer.recv_into
                    recv_flags = zlink.RecvFlags.DONT_WAIT

                    def drain_ready():
                        # C receive_one_message + drain_non_blocking_messages:
                        # every matched header counts; latency excludes
                        # clock-skew (latency_ns_from_message returns None).
                        nonlocal count
                        while True:
                            if not recv_into(recv_storage, flags=recv_flags):
                                return
                            with recv_storage:
                                if not recv_storage.parts:
                                    continue
                                data = recv_storage.parts[-1].data
                                if len(data) == len(STOP_TOKEN) and data == STOP_TOKEN:
                                    continue
                                active, latency = active_message_latency_ns(
                                    data,
                                    expected_msg_size=args.msg_size,
                                    run_id=run_id,
                                )
                                if not active:
                                    continue
                                count += 1
                                if latency is not None:
                                    latency_sampler.add(latency)

                    while not stop_event.is_set():
                        remaining_ms = int(
                            (active_deadline - time.perf_counter()) * 1000
                        )
                        if remaining_ms <= 0:
                            break
                        ready_count = safe_poll(poller, poll_events, max(1, remaining_ms))
                        if ready_count:
                            for offset in range(ready_count):
                                if poll_events.revents(offset) & int(
                                    zlink.PollEventFlag.POLLIN
                                ):
                                    drain_ready()
                        if time.perf_counter() >= active_deadline:
                            break

                    # C drain_phase_until_idle: bounded uncounted tail drain
                    # so late in-flight messages do not skew the next case.
                    idle_deadline = time.perf_counter() + 0.05
                    tail_deadline = time.perf_counter() + active_duration_s
                    while (
                        not stop_event.is_set()
                        and time.perf_counter() < tail_deadline
                    ):
                        if recv_into(recv_storage, flags=recv_flags):
                            with recv_storage:
                                pass
                            idle_deadline = time.perf_counter() + 0.05
                            continue
                        if time.perf_counter() >= idle_deadline:
                            break
                        safe_poll(poller, poll_events, 50)

                    if count <= 0:
                        raise RuntimeError(
                            "multi dealer-dealer server did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=count,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latency_sampler=latency_sampler,
                    )
                    print_result_lines(
                        "MULTI_DEALER_DEALER",
                        args.transport,
                        args.msg_size,
                        metrics,
                    )


if __name__ == "__main__":
    main()
