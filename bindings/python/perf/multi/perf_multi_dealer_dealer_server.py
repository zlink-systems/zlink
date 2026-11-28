import sys
import threading
import time

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    benchmark_run_id,
    configure_multi_tls_server,
    LatencySampler,
    print_result_lines,
    print_multi_auto_hwm_detail,
    parse_server_args,
    perf_server_context,
    resolve_multi_monitor_hwm_bytes,
    result_metrics,
    received_has_stop_token,
    received_metric_payload,
    safe_poll,
    resolve_multi_send_drain_timeout_ms,
)

from perf_multi_measurement import (
    NS_PER_SECOND, SETTLE_SECONDS, latency_duration, record_receive, wait_ms,
)


def dealer_dealer_active_poll_timeout_ms(
    stop_token_seen, active_deadline, *, now=None
):
    if not stop_token_seen:
        return -1
    current = time.perf_counter() if now is None else now
    return max(1, int((active_deadline - current) * 1000))


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
            with dealer.monitor_open(
                zlink.MonitorEventMask.CONNECTION_READY,
                resolve_multi_monitor_hwm_bytes(),
            ) as monitor:
                dealer.bind(endpoint)
                print(f"READY,{endpoint}", flush=True)
                # The socket supports one public monitor owner. Release the
                # readiness monitor before the measured-path HWM snapshot
                # opens its short-lived monitor.
                monitor.close()
                with zlink.create_poller() as poller:
                    poller.add_socket(dealer, zlink.PollEventFlag.POLLIN, 0)
                    poll_events = zlink.create_poll_events(1)
                    start_event.wait()
                    if stop_event.is_set():
                        return
                    ctx.recalculate_auto_hwm()
                    recv_storage = zlink.create_received()
                    drain_seconds = resolve_multi_send_drain_timeout_ms() / 1000
                    sampler = LatencySampler()

                    def receive(seconds, paced=False):
                        start = time.monotonic()
                        end = start + seconds
                        deadline = int(end * NS_PER_SECOND)
                        sample_start = int((start + SETTLE_SECONDS) * NS_PER_SECOND) if paced else None
                        drain_end = end + drain_seconds
                        remaining = args.clients
                        count = 0
                        while remaining and not stop_event.is_set():
                            now = time.monotonic()
                            if now >= drain_end:
                                raise TimeoutError("multi dealer-dealer receive drain timed out")
                            poll_ms = (
                                dealer_dealer_active_poll_timeout_ms(remaining != args.clients, end, now=now)
                                if now < end and not paced else wait_ms(now, drain_end)
                            )
                            if not safe_poll(poller, poll_events, poll_ms):
                                continue
                            while remaining and time.monotonic() < drain_end:
                                if not dealer.recv_into(recv_storage, flags=zlink.RecvFlags.DONT_WAIT):
                                    break
                                observed = time.monotonic_ns()
                                with recv_storage:
                                    if received_has_stop_token(recv_storage):
                                        remaining -= 1
                                        continue
                                    data = received_metric_payload(recv_storage, expected_size=args.msg_size)
                                    if data:
                                        count += record_receive(
                                            data, args.msg_size, run_id, observed, deadline,
                                            sampler if paced else None, sample_start=sample_start,
                                        )
                        return count

                    count = receive(active_duration_s)
                    if not count:
                        raise RuntimeError("multi dealer-dealer received no active messages")
                    print(f"PHASE_DRAINED,{args.msg_size},{count}", flush=True)
                    paced = receive(SETTLE_SECONDS + latency_duration(args.duration), paced=True)
                    print(f"PHASE_COMPLETE,{args.msg_size},{paced}", flush=True)
                    if sampler.count == 0:
                        raise RuntimeError("multi dealer-dealer received no paced messages")
                    print_multi_auto_hwm_detail(dealer, "endpoint", args.transport, args.msg_size, "dealer")
                    metrics = result_metrics(
                        count=count, msg_size=args.msg_size, elapsed_s=args.duration,
                        latency_sampler=sampler,
                    )
                    print_result_lines("DEALER_DEALER", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
