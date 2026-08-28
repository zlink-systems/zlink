import sys
import threading
import time

import zlink

from perf_common import (
    STOP_TOKEN,
    apply_single_socket_options,
    benchmark_run_id,
    configure_single_tls_client,
    configure_single_tls_server,
    new_payload,
    parse_single_args,
    perf_context,
    poll_idle_ms,
    print_result_lines,
    new_single_latency_sampler,
    run_one_way_receiver,
    result_metrics,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    single_routing_probe,
    send_routed_sync,
    stamp_payload,
    wait_monitor_event,
)


def _send_stop_token(sock):
    """PERF_SINGLE_TEST_POLICY § 1.4 wire-level shutdown signal."""

    for _ in range(100):
        try:
            sock.send().message(STOP_TOKEN).submit_sync(flags=zlink.SendFlags.NONE)
            return
        except zlink.SubmitError as exc:
            if exc.result != zlink.SubmitResult.BACKPRESSURED:
                raise
            poll_idle_ms(1)


def _public_one_way_metrics(sender, receiver, *, msg_size, duration_s, run_id):
    return None


def main(argv=None):
    args = parse_single_args(argv or sys.argv[1:], pattern="dealer_router")
    run_id = benchmark_run_id()
    latency_sampler = new_single_latency_sampler()
    received = 0
    payload = new_payload(args.msg_size)

    def send_loop(dealer, active_end):
        # Core owns blocking HWM admission on this dedicated sender thread.
        stamp = stamp_payload
        while time.perf_counter() < active_end:
            send_routed_sync(
                dealer, stamp(payload, phase=1, run_id=run_id)
            )
        _send_stop_token(dealer)

    with perf_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            with zlink.create_dealer_socket(ctx) as dealer:
                endpoint = resolve_single_endpoint(args.transport, "dealer-router")
                apply_single_socket_options(router, dealer)
                configure_single_tls_server(router, args.transport)
                configure_single_tls_client(dealer, args.transport)
                with dealer.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as monitor:
                    router.bind(endpoint)
                    dealer.connect(endpoint)
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_single_connect_ready_timeout_ms(),
                    )

                # C perf_dealer_router.cpp wait_for_dealer_router_ready:
                # one-shot DEALER->ROUTER routing probe before phase=active.
                if not single_routing_probe(
                    dealer,
                    router,
                    payload,
                    run_id=run_id,
                    msg_size=args.msg_size,
                ):
                    raise RuntimeError(
                        "dealer-router routing probe did not establish route"
                    )

                active_end = time.perf_counter() + args.duration
                metrics = _public_one_way_metrics(
                    dealer,
                    router,
                    msg_size=args.msg_size,
                    duration_s=args.duration,
                    run_id=run_id,
                )
                if metrics is None:
                    sender = threading.Thread(
                        target=send_loop, args=(dealer, active_end), daemon=True
                    )
                    sender.start()
                    # C perf_dealer_router.cpp run_active_phase receiver.
                    received = run_one_way_receiver(
                        router,
                        method="recv",
                        msg_size=args.msg_size,
                        run_id=run_id,
                        active_end=active_end,
                        received=received,
                        latency_sampler=latency_sampler,
                    )

                    sender.join()
                    if received == 0:
                        raise RuntimeError(
                            "dealer-router benchmark did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=received,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latency_sampler=latency_sampler,
                    )
                print_result_lines("DEALER_ROUTER", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
