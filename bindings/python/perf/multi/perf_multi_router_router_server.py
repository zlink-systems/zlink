import asyncio
import sys
import threading

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    measurement_part_count,
    parse_server_args,
    perf_server_context,
    recv_nonblocking,
    RelaySchedulerQuantum,
    RoutedReplySender,
    safe_poll,
    scoped_relay_eager_task_factory,
)


async def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-router-router")
    stop = threading.Event()
    scheduler_quantum = RelaySchedulerQuantum()

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
                replies = RoutedReplySender(router)
                # Python 3.12 starts each reply task through its public
                # coroutine immediately. Older runtimes retain create_task
                # scheduling and receive a bounded turn every quantum.
                with scoped_relay_eager_task_factory():
                    while not stop.is_set():
                        replies.raise_if_failed()
                        # The application FIFO may keep receiving snapshots,
                        # but its single sender awaits the preceding Core
                        # admission before submitting the next reply.
                        ready_count = safe_poll(poller, poll_events, 0)
                        for offset in range(ready_count):
                            if poll_events.slot(offset) != 0 or not (
                                poll_events.revents(offset)
                                & int(zlink.PollEventFlag.POLLIN)
                            ):
                                continue
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
                                replies.enqueue(payload, routing_id)
                                # This is a scheduler fairness budget only. It
                                # neither caps pending replies nor gates Core
                                # admission on their count.
                                if scheduler_quantum.received():
                                    await asyncio.sleep(0)
                        scheduler_quantum.reset()
                        await asyncio.sleep(0)
                await replies.drain()


if __name__ == "__main__":
    asyncio.run(main())
