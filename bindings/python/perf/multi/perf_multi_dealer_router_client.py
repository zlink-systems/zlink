import asyncio
import os
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    active_message_latency_ns,
    benchmark_run_id,
    configure_multi_tls_client,
    LatencySampler,
    new_payload,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    recv_nonblocking,
    received_metric_payload,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    safe_poll,
    send_routed,
    stamp_payload,
    wait_monitor_event,
)


async def main(argv=None):
    args = parse_client_args(argv or sys.argv[1:], pattern="dealer_router")
    run_id = benchmark_run_id()
    payloads = [new_payload(args.msg_size) for _ in range(args.clients)]
    received = 0
    latency_sampler = LatencySampler()
    seq = 0

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

                active_deadline = time.perf_counter() + args.duration
                recv_storage = [zlink.create_received() for _ in sockets]
                with zlink.create_poller() as poller:
                    poll_events = zlink.create_poll_events(max(1, len(sockets)))
                    for index, sock in enumerate(sockets):
                        poller.add_socket(
                            sock,
                            zlink.PollEventFlag.POLLIN,
                            index,
                        )

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

                    send_tasks = [
                        asyncio.create_task(send_loop(index, current_sock))
                        for index, current_sock in enumerate(sockets)
                    ]
                    send_completion = asyncio.gather(*send_tasks)
                    send_drain_deadline = active_deadline + max(
                        0.001,
                        float(
                            os.environ.get(
                                "PERF_MULTI_SEND_DRAIN_TIMEOUT_MS", "1000"
                            )
                        )
                        / 1000.0,
                    )
                    while time.perf_counter() < active_deadline or (
                        not send_completion.done()
                        and time.perf_counter() < send_drain_deadline
                    ):
                        if (
                            send_completion.done()
                            and send_completion.exception() is not None
                        ):
                            raise send_completion.exception()
                        wait_deadline = (
                            active_deadline
                            if time.perf_counter() < active_deadline
                            else send_drain_deadline
                        )
                        remaining_ms = int(
                            (wait_deadline - time.perf_counter()) * 1000
                        )
                        if remaining_ms <= 0:
                            break
                        # Public send awaitables drive admission progress.
                        # Receive probing must not block their event loop on a
                        # periodic timer.
                        ready_count = safe_poll(poller, poll_events, 0)
                        for offset in range(ready_count):
                            index = poll_events.slot(offset)
                            if index < 0 or index >= len(sockets):
                                continue
                            current_sock = sockets[index]
                            ev = poll_events.revents(offset)
                            if not (ev & int(zlink.PollEventFlag.POLLIN)):
                                continue
                            while True:
                                msg = recv_nonblocking(
                                    current_sock,
                                    storage=recv_storage[index],
                                )
                                if msg is None:
                                    break
                                with msg:
                                    active = False
                                    latency = None
                                    data = received_metric_payload(
                                        msg, expected_size=args.msg_size
                                    )
                                    if data:
                                        active, latency = active_message_latency_ns(
                                            data, expected_msg_size=args.msg_size,
                                            run_id=run_id,
                                        )
                                    # C: every matched header counts (++local_recv,
                                    # -> recv_count); latency only added when not
                                    # clock-skewed (sent_ts_ns>0 && now>=sent_ts),
                                    # halved for the round trip.
                                    if active and time.perf_counter() < active_deadline:
                                        received += 1
                                        if latency is not None:
                                            latency_sampler.add(latency / 2.0)
                        # Drain every reply that is already ready before
                        # yielding to the per-socket send loops. Yielding once
                        # per message gives this single receive coordinator
                        # only one turn for every full round of send tasks.
                        await asyncio.sleep(0)
                    if not send_completion.done():
                        send_completion.cancel()
                        await asyncio.gather(send_completion, return_exceptions=True)
                        raise RuntimeError(
                            "multi dealer-router send admission drain timed out"
                        )
                    await send_completion
                if received == 0:
                    raise RuntimeError(
                        "multi dealer-router benchmark did not receive any active reply"
                    )
                if latency_sampler.count == 0:
                    raise RuntimeError(
                        "multi dealer-router benchmark received active replies without latency samples"
                    )
                metrics = result_metrics(
                    count=received,
                    msg_size=args.msg_size,
                    elapsed_s=args.duration,
                    latency_sampler=latency_sampler,
                    bandwidth_multiplier=2.0,
                )
                print_result_lines(
                    "MULTI_DEALER_ROUTER_SENDSEND",
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
