import asyncio
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
    run_one_way_receiver_public_recv,
    send_routed,
    result_metrics,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    stamp_payload,
    wait_monitor_event,
)


async def _send_stop_token(sock):
    """Send wire-level stop token once with bounded backpressure attempts.

    PERF_SINGLE_TEST_POLICY § 1.4: phase end is signalled on the wire by
    sending ``__zlink_perf_stop__`` once. The send is allowed to block
    through transient backpressure (no deadline) so that the receiver
    always observes the terminator after every in-flight payload.
    """

    for _ in range(100):
        try:
            await sock.send().message(STOP_TOKEN).submit()
            return
        except zlink.SubmitError as exc:
            if exc.result != zlink.SubmitResult.BACKPRESSURED:
                raise
            poll_idle_ms(1)


def _public_one_way_metrics(sender, receiver, *, msg_size, duration_s, run_id):
    return None


async def main(argv=None):
    args = parse_single_args(argv or sys.argv[1:], pattern="dealer_dealer")
    run_id = benchmark_run_id()
    latencies = []
    received = 0
    payload = new_payload(args.msg_size)

    async def send_loop(dealer, active_end):
        # Preserve C's fresh timestamp per attempt while the routed terminal
        # suspends this coroutine until Core admission.
        while time.perf_counter() < active_end:
            await send_routed(
                dealer, stamp_payload(payload, phase=1, run_id=run_id)
            )
        # PERF_SINGLE_TEST_POLICY § 1.4: signal phase end on the wire.
        await _send_stop_token(dealer)

    with perf_context() as ctx:
        with zlink.create_dealer_socket(ctx) as server:
            with zlink.create_dealer_socket(ctx) as client:
                endpoint = resolve_single_endpoint(args.transport, "dealer-dealer")
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
                    sender = threading.Thread(target=lambda: asyncio.run(
                        send_loop(client, active_end)), daemon=True)
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
                    if received == 0:
                        raise RuntimeError(
                            "dealer-dealer benchmark did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=received,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latencies_ns=latencies,
                    )
                print_result_lines("DEALER_DEALER", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    asyncio.run(main())
