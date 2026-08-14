import asyncio
import sys
import threading
from collections import deque

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    parse_server_args,
    perf_server_context,
    recv_nonblocking,
    safe_poll,
    send_routed,
)


async def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-router-router")
    stop = threading.Event()
    pending = deque()

    def wait_stop():
        for line in sys.stdin:
            if line.strip().upper() in {"STOP", "QUIT"}:
                stop.set()
                return
        stop.set()

    threading.Thread(target=wait_stop, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            router.set_routing_id(b"SERVER")
            configure_multi_tls_server(router, args.transport)
            apply_multi_socket_options(router)
            router.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)
            with zlink.create_poller() as poller:
                poller.add_socket(router, zlink.PollEventFlag.POLLIN, 0)
                poll_events = zlink.create_poll_events(1)
                recv_storage = zlink.create_received()
                poll_interest = zlink.PollEventFlag.POLLIN
                # Match the C relay server: observe POLLOUT only while a
                # backpressured reply is queued, and bound the idle wait so
                # the stdin stop event is observed.
                aux_wait_ms = 100
                while not stop.is_set():
                    while pending:
                        routing_id, payload = pending[0]
                        if not await send_routed(
                            router, payload, routing_id=routing_id
                        ):
                            break
                        pending.popleft()
                    while True:
                        received = recv_nonblocking(router, storage=recv_storage)
                        if received is None:
                            break
                        with received:
                            payload = received.parts[0].data
                            routing_id = None
                            sent = False
                            if not pending:
                                sent = (
                                    received.send()
                                    .message(payload)
                                    .flags(zlink.SendFlags.DONT_WAIT)
                                    .submit()
                                )
                            if not sent:
                                routing_id = bytes(received.routing_id)
                                payload = bytes(payload)
                        if not sent:
                            pending.append((routing_id, payload))
                    next_interest = zlink.PollEventFlag.POLLIN
                    if pending:
                        next_interest |= zlink.PollEventFlag.POLLOUT
                    if next_interest != poll_interest:
                        poller.modify_socket(router, next_interest)
                        poll_interest = next_interest
                    safe_poll(poller, poll_events, aux_wait_ms)


if __name__ == "__main__":
    asyncio.run(main())
