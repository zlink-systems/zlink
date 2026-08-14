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
    run_one_way_receiver,
    result_metrics,
    resolve_single_endpoint,
    resolve_single_connect_ready_timeout_ms,
    single_routing_probe,
    send_routed,
    stamp_payload,
    wait_monitor_event,
)


async def _send_router_stop_token(router, dest_routing_id):
    """PERF_SINGLE_TEST_POLICY § 1.4 wire-level shutdown signal.

    Router-router uses ``send(routing_id).message(payload).submit()``; the stop token is a
    single payload frame addressed to the peer.
    """

    for _ in range(100):
        try:
            await router.send(dest_routing_id).message(STOP_TOKEN).submit()
            return
        except zlink.SubmitError as exc:
            if exc.result not in (
                zlink.SubmitResult.BACKPRESSURED,
                zlink.SubmitResult.NOT_CONNECTED,
            ):
                raise
            poll_idle_ms(1)


def _public_one_way_metrics(sender, receiver, *, msg_size, duration_s, run_id):
    return None


async def main(argv=None):
    args = parse_single_args(argv or sys.argv[1:], pattern="router_router")
    run_id = benchmark_run_id()
    latencies = []
    received = 0
    payload = new_payload(args.msg_size)

    async def send_loop(router, active_end):
        # Preserve C's fresh timestamp per attempt while the routed terminal
        # suspends this coroutine until Core admission.
        stamp = stamp_payload
        submit_backpressured = zlink.SubmitResult.BACKPRESSURED
        submit_not_connected = zlink.SubmitResult.NOT_CONNECTED
        while time.perf_counter() < active_end:
            try:
                await send_routed(
                    router,
                    stamp(payload, phase=1, run_id=run_id),
                    routing_id=b"SERVER",
                )
            except zlink.SubmitError as exc:
                if exc.result not in (submit_backpressured, submit_not_connected):
                    raise
        await _send_router_stop_token(router, b"SERVER")

    with perf_context() as ctx:
        with zlink.create_router_socket(ctx) as server:
            with zlink.create_router_socket(ctx) as client:
                server.set_routing_id(b"SERVER")
                client.set_routing_id(b"CLIENT")
                client.router_options.connect_routing_id = b"SERVER"
                endpoint = resolve_single_endpoint(args.transport, "router-router")
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

                # C perf_router_router.cpp wait_for_router_router_ready:
                # one-shot routed probe (addressed to the peer routing id)
                # before phase=active.
                if not await single_routing_probe(
                    client,
                    server,
                    payload,
                    run_id=run_id,
                    msg_size=args.msg_size,
                    routing_id=b"SERVER",
                ):
                    raise RuntimeError(
                        "router-router routing probe did not establish route"
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
                    # C perf_router_router.cpp run_active_phase receiver.
                    received = run_one_way_receiver(
                        server,
                        method="recv",
                        msg_size=args.msg_size,
                        run_id=run_id,
                        active_end=active_end,
                        received=received,
                        latencies=latencies,
                    )

                    sender.join()
                    if received == 0:
                        raise RuntimeError(
                            "router-router benchmark did not receive any active message"
                        )
                    metrics = result_metrics(
                        count=received,
                        msg_size=args.msg_size,
                        elapsed_s=args.duration,
                        latencies_ns=latencies,
                    )
                print_result_lines("ROUTER_ROUTER", args.transport, args.msg_size, metrics)


if __name__ == "__main__":
    asyncio.run(main())
