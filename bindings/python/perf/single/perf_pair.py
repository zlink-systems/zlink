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
    run_one_way_receiver_public_recv,
    send_routed_sync,
    result_metrics,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    stamp_payload,
    wait_monitor_event,
)


def _send_stop_token(sock):
    """PERF_SINGLE_TEST_POLICY § 1.4 wire-level shutdown signal.

    Single perf uses only the synchronous blocking public send terminal.
    """

    for _ in range(100):
        try:
            sock.send().message(STOP_TOKEN).submit_sync()
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
    latency_sampler = new_single_latency_sampler()
    received = 0

    def send_loop(client, active_end):
        # Core owns blocking HWM admission on this dedicated sender thread.
        while time.perf_counter() < active_end:
            send_routed_sync(
                client, stamp_payload(payload, phase=1, run_id=run_id)
            )
        # PERF_SINGLE_TEST_POLICY § 1.4: wire stop token instead of
        # threading.Event coordination.
        _send_stop_token(client)

    with perf_context() as ctx:
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
                    sender_errors = []

                    def run_sender():
                        try:
                            send_loop(client, active_end)
                        except BaseException as exc:  # pragma: no cover - surfaced on main thread
                            sender_errors.append(exc)

                    sender = threading.Thread(
                        target=run_sender, daemon=True
                    )
                    sender.start()
                    # C perf_single_one_way.hpp run_active_phase receiver.
                    received = run_one_way_receiver_public_recv(
                        server,
                        msg_size=args.msg_size,
                        run_id=run_id,
                        active_end=active_end,
                        received=received,
                        latency_sampler=latency_sampler,
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
                        latency_sampler=latency_sampler,
                    )
                print_result_lines("PAIR", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    main()
