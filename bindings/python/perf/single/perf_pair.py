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
    run_one_way_receiver_public_recv,
    send_nonblocking,
    result_metrics,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    stamp_payload,
    wait_monitor_event,
)


def _send_stop_token(sock):
    """PERF_SINGLE_TEST_POLICY § 1.4 wire-level shutdown signal."""

    for _ in range(100):
        try:
            sock.send().message(STOP_TOKEN).submit()
            return
        except zlink.SubmitError as exc:
            if exc.result != zlink.SubmitResult.BACKPRESSURED:
                raise
            poll_idle_ms(1)


def _public_one_way_metrics(sender, receiver, *, msg_size, duration_s, run_id):
    return None


def main(argv=None):
    args = parse_single_args(argv or sys.argv[1:], pattern="pair")
    payload = new_payload(args.msg_size)
    run_id = benchmark_run_id()
    latencies = []
    received = 0
    sender_errors = []

    def send_loop(client, active_end):
        try:
            # C perf_single_one_way.hpp send_active_samples: DONTWAIT send,
            # re-stamp the payload (fresh now_ns) on every retry, busy-loop
            # through transient backpressure (no blocking submit).
            while time.perf_counter() < active_end:
                send_nonblocking(
                    client, stamp_payload(payload, phase=1, run_id=run_id)
                )
            # PERF_SINGLE_TEST_POLICY § 1.4: wire stop token instead of
            # threading.Event coordination.
            _send_stop_token(client)
        except BaseException as exc:  # pragma: no cover - surfaced on main thread
            sender_errors.append(exc)

    with perf_context() as ctx:
        apply_single_auto_hwm_msg_unit(ctx, args.msg_size)
        with zlink.create_pair_socket(ctx) as server:
            with zlink.create_pair_socket(ctx) as client:
                endpoint = resolve_single_endpoint(args.transport, "pair")
                apply_single_socket_options(server, client)
                configure_single_tls_server(server, args.transport)
                configure_single_tls_client(client, args.transport)
                with client.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as monitor:
                    server.bind(endpoint)
                    client.connect(endpoint)
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_single_connect_ready_timeout_ms(),
                    )

                active_end = time.perf_counter() + args.duration
                metrics = _public_one_way_metrics(
                    client,
                    server,
                    msg_size=args.msg_size,
                    duration_s=args.duration,
                    run_id=run_id,
                )
                if metrics is None:
                    sender = threading.Thread(
                        target=send_loop, args=(client, active_end), daemon=True
                    )
                    sender.start()
                    # C perf_single_one_way.hpp run_active_phase receiver.
                    received = run_one_way_receiver_public_recv(
                        server,
                        msg_size=args.msg_size,
                        run_id=run_id,
                        active_end=active_end,
                        received=received,
                        latencies=latencies,
                    )

                    sender.join()
                    if sender_errors:
                        raise sender_errors[0]
                    if received == 0:
                        raise RuntimeError(
                            "pair benchmark did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=received,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latencies_ns=latencies,
                    )
                print_result_lines("PAIR", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
