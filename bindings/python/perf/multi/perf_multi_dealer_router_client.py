import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    print_multi_auto_hwm_detail,
    resolve_multi_connect_ready_timeout_ms,
    wait_monitor_event,
)

from perf_multi_sendsend import run_echo


async def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="dealer_router")
    run_id = benchmark_run_id()

    with perf_client_context() as ctx:
        sockets = [zlink.create_dealer_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for index, sock in enumerate(sockets):
                    sock.set_routing_id(f"CLIENT-{index}".encode("ascii"))
                    monitor = stack.enter_context(
                        sock.monitor_open(
                            zlink.MonitorEventMask.CONNECTION_READY,
                            resolve_multi_monitor_hwm_bytes(),
                        )
                    )
                    configure_multi_tls_client(sock, args.transport)
                    apply_multi_socket_options(sock)
                    sock.connect(args.endpoint)
                    monitors.append(monitor)
                for monitor in monitors:
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_multi_connect_ready_timeout_ms(),
                    )
                stack.close()
                ctx.recalculate_auto_hwm()

                active_deadline = time.monotonic() + args.duration
                metrics = await run_echo(args, "DEALER_ROUTER_SENDSEND", sockets, run_id, active_deadline)
                if sockets:
                    print_multi_auto_hwm_detail(
                        sockets[0], "endpoint", args.transport, args.msg_size, "dealer"
                    )
                print_result_lines(
                    "DEALER_ROUTER_SENDSEND",
                    args.transport,
                    args.msg_size,
                    metrics,
                )
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
