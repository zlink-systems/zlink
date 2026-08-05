import sys
import threading
import time

import zlink

from perf_common import (
    STOP_TOKEN,
    apply_single_auto_hwm_msg_unit,
    apply_single_socket_options,
    benchmark_run_id,
    configure_single_tls_client,
    configure_single_tls_server,
    new_payload,
    parse_single_args,
    perf_context,
    poll_idle_ms,
    print_result_lines,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    resolve_single_pubsub_ready_settle_s,
    resolve_single_pubsub_recv_timeout_ms,
    result_metrics,
    run_one_way_subscriber_public_subscribe,
    stamp_payload,
    wait_monitor_event,
)


TOPIC = b"bench"


def _publish_stop_token(publisher):
    """PERF_SINGLE_TEST_POLICY § 1.4 wire-level shutdown signal."""

    for _ in range(100):
        try:
            publisher.publish(TOPIC).message(STOP_TOKEN).submit()
            return
        except zlink.SubmitError as exc:
            if exc.result != zlink.SubmitResult.BACKPRESSURED:
                raise
            poll_idle_ms(1)


def _public_one_way_metrics(sender, receiver, *, msg_size, duration_s, run_id):
    return None


def main(argv=None):
    args = parse_single_args(argv or sys.argv[1:], pattern="pubsub")
    payload = new_payload(args.msg_size)
    run_id = benchmark_run_id()
    latencies = []
    received = 0

    def send_loop(publisher, active_end):
        # C send_active_samples: DONTWAIT publish, re-stamp fresh now_ns on
        # every retry, busy-loop through transient backpressure.
        flag = int(zlink.SendFlags.DONT_WAIT)
        publish = publisher.publish
        topic = TOPIC
        stamp = stamp_payload
        perf_counter = time.perf_counter
        submit_backpressured = zlink.SubmitResult.BACKPRESSURED
        while perf_counter() < active_end:
            try:
                publish(topic).message(stamp(payload, phase=1, run_id=run_id)).flags(
                    flag
                ).submit()
            except zlink.SubmitError as exc:
                if exc.result != submit_backpressured:
                    raise
        _publish_stop_token(publisher)
    with perf_context() as ctx:
        apply_single_auto_hwm_msg_unit(ctx, args.msg_size)
        with zlink.create_pub_socket(ctx) as publisher:
            # Match C perf: the harness always sets NODROP explicitly. The
            # socket default is lossy fanout, which would drop samples and the
            # stop token.
            publisher.pub_options.no_drop = True
            with zlink.create_sub_socket(ctx) as subscriber:
                endpoint = resolve_single_endpoint(args.transport, "pubsub")
                apply_single_socket_options(
                    publisher,
                    subscriber,
                    receive_timeout_ms=resolve_single_pubsub_recv_timeout_ms(),
                )
                configure_single_tls_server(publisher, args.transport)
                configure_single_tls_client(subscriber, args.transport)
                subscriber.set_subscription(TOPIC)
                with subscriber.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as monitor:
                    publisher.bind(endpoint)
                    subscriber.connect(endpoint)
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_single_connect_ready_timeout_ms(),
                    )
                wait_seconds = resolve_single_pubsub_ready_settle_s()
                if wait_seconds > 0:
                    time.sleep(wait_seconds)

                active_end = time.perf_counter() + args.duration
                metrics = _public_one_way_metrics(
                    publisher,
                    subscriber,
                    msg_size=args.msg_size,
                    duration_s=args.duration,
                    run_id=run_id,
                )
                if metrics is None:
                    sender = threading.Thread(
                        target=send_loop, args=(publisher, active_end), daemon=True
                    )
                    sender.start()
                    # C perf_pubsub.cpp run_active_phase receiver.
                    received = run_one_way_subscriber_public_subscribe(
                        subscriber,
                        topic=TOPIC,
                        msg_size=args.msg_size,
                        run_id=run_id,
                        active_end=active_end,
                        received=received,
                        latencies=latencies,
                    )

                    sender.join()
                    if received == 0:
                        raise RuntimeError(
                            "pubsub benchmark did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=received,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latencies_ns=latencies,
                    )
                print_result_lines("PUBSUB", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
