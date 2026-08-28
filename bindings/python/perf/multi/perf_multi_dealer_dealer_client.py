import asyncio
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    new_payload,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    resolve_multi_connect_ready_timeout_ms,
    send_routed,
    stamp_payload,
    wait_monitor_event,
)


async def _send_stop_token(sock):
    await send_routed(sock, STOP_TOKEN, measurement=False)


async def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="dealer_dealer")
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    seq = 0

    with perf_client_context() as ctx:
        sockets = [zlink.create_dealer_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for sock in sockets:
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
                print(f"CLIENT_READY,{args.msg_size}", flush=True)
                command = sys.stdin.readline().strip()
                if command != f"START,{args.msg_size}":
                    raise SystemExit(f"unexpected command: {command}")

                active_deadline = time.perf_counter() + args.duration
                async def send_loop(index, current_sock):
                    nonlocal seq
                    while time.perf_counter() < active_deadline:
                        seq += 1
                        await send_routed(
                            current_sock,
                            stamp_payload(
                                payloads[index], phase=1, run_id=run_id, seq=seq
                            ),
                        )
                    await _send_stop_token(current_sock)
                await asyncio.gather(*(
                    send_loop(index, sock) for index, sock in enumerate(sockets)
                ))
                # C run_single_size_case: send a wire stop token per socket
                # so the server receive window terminates.
                print(f"CLIENT_DONE,{args.msg_size}", flush=True)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    asyncio.run(main())
