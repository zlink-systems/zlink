import asyncio
import sys
import threading

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    parse_server_args,
    perf_server_context,
    recv_nonblocking,
    safe_poll,
    measurement_parts,
    measurement_part_count,
)


async def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-router-router")
    stop = threading.Event()
    # ROUTER routed send is HWM-managed and ASYNC-classified (Core
    # `zlink_send_async`, bindings/doc/spec/async-coroutine-policy.ko.md):
    # `submit()` returns an awaitable completed by Core's send-completion
    # notification. The binding owns no retry queue or POLLOUT
    # readiness-hint (`send_ready` semantics is abolished), so each reply is
    # a fire-and-forget task instead of a manually-queued DONTWAIT retry
    # driven by POLLOUT.
    pending_tasks = set()
    send_errors = []

    def _on_send_done(task):
        pending_tasks.discard(task)
        if task.cancelled():
            return
        exc = task.exception()
        if exc is not None:
            send_errors.append(exc)

    def wait_stop():
        for line in sys.stdin:
            if line.strip().upper() in {"STOP", "QUIT"}:
                stop.set()
                return
        # stdin EOF (parent closed pipe) is also a STOP signal.
        stop.set()

    threading.Thread(target=wait_stop, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            configure_multi_tls_server(router, args.transport)
            apply_multi_socket_options(router)
            router.set_routing_id(b"SERVER")
            router.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)
            with zlink.create_poller() as poller:
                poller.add_socket(router, zlink.PollEventFlag.POLLIN, 0)
                poll_events = zlink.create_poll_events(1)
                recv_storage = zlink.create_received()
                # Small bounded wait: lets the stdin stop event terminate an
                # otherwise idle server and keeps the event loop responsive
                # to Core's send-completion notifications for pending reply
                # tasks.
                aux_wait_ms = 5
                while not stop.is_set():
                    if send_errors:
                        raise send_errors[0]
                    while True:
                        received = recv_nonblocking(router, storage=recv_storage)
                        if received is None:
                            break
                        with received:
                            if len(received.parts) != measurement_part_count():
                                raise RuntimeError("invalid measured multipart request")
                            if len(received.parts) == 2 and len(received.parts[1].data) != 0:
                                raise RuntimeError("invalid measured multipart trailing frame")
                            payload = bytes(received.parts[0].data)
                            routing_id = bytes(received.routing_id)
                        task = asyncio.create_task(
                            router.send(routing_id).messages(*measurement_parts(payload)).submit()
                        )
                        pending_tasks.add(task)
                        task.add_done_callback(_on_send_done)
                    safe_poll(poller, poll_events, aux_wait_ms)
                    await asyncio.sleep(0)
                if send_errors:
                    raise send_errors[0]
                if pending_tasks:
                    await asyncio.gather(*pending_tasks, return_exceptions=True)
                if send_errors:
                    raise send_errors[0]


if __name__ == "__main__":
    asyncio.run(main())
