import time

import zlink

from perf_multi_common import (
    LatencySampler,
    MultiSendTurnCoordinator,
    RelaySchedulerQuantum,
    new_payload,
    received_metric_payload,
    recv_nonblocking,
    resolve_multi_send_drain_timeout_ms,
    result_metrics,
    stamp_payload,
    submit_routed,
)
from perf_multi_measurement import (
    MAX_BURST_PER_SENDER,
    NS_PER_SECOND,
    PHASE_ACTIVE,
    SETTLE_SECONDS,
    Schedule,
    latency_duration,
    record_receive,
    wait_ms,
)


async def run_echo(args, pattern, sockets, run_id, active_deadline, *, routing_id=None):
    payloads = [new_payload(args.msg_size) for _ in sockets]
    stores = [zlink.create_received() for _ in sockets]
    with zlink.create_poller() as poller:
        events = zlink.create_poll_events(max(1, len(sockets)))
        for index, sock in enumerate(sockets):
            poller.add_socket(
                sock, zlink.PollEventFlag.POLLIN | zlink.PollEventFlag.POLLCOMPLETION, index
            )

        async def run(deadline, schedule=None, sampler=None):
            received = 0
            replies = RelaySchedulerQuantum(MAX_BURST_PER_SENDER)
            deadline_ns = int(deadline * NS_PER_SECOND)
            sample_start = None if schedule is None else int((schedule.epoch + SETTLE_SECONDS) * NS_PER_SECOND)
            coordinator = MultiSendTurnCoordinator(poller, events, len(sockets))

            def submit(index):
                phase = PHASE_ACTIVE if schedule is None else schedule.phase(time.monotonic())
                return submit_routed(
                    sockets[index],
                    stamp_payload(payloads[index], phase=phase, run_id=run_id),
                    routing_id=routing_id,
                )

            def drain(ready_count):
                nonlocal received
                for offset in range(ready_count):
                    index = events.slot(offset)
                    if index >= len(sockets) or not events.revents(offset) & int(zlink.PollEventFlag.POLLIN):
                        continue
                    for _ in range(MAX_BURST_PER_SENDER):
                        msg = recv_nonblocking(sockets[index], storage=stores[index])
                        if msg is None:
                            break
                        observed = time.monotonic_ns()
                        with msg:
                            data = received_metric_payload(msg, expected_size=args.msg_size)
                            if data and record_receive(
                                data, args.msg_size, run_id, observed, deadline_ns, sampler, 2,
                                sample_start=sample_start,
                            ):
                                received += 1
                        if schedule is not None and replies.received():
                            coordinator._submit_round(deadline, submit)

            drain_timeout_ms = max(resolve_multi_send_drain_timeout_ms(), args.duration * 3000)
            active_sends = await coordinator.run(
                deadline, submit, dispatch=drain, schedule=schedule,
                drain_timeout_ms=drain_timeout_ms,
            )
            # Admission completion and receipt of the last reply are separate.
            # Keep polling the same sockets until the reply stream is quiet.
            drain_end = deadline + drain_timeout_ms / 1000
            quiet_end = time.monotonic() + 0.05
            while (now := time.monotonic()) < min(drain_end, quiet_end):
                timeout = wait_ms(now, min(drain_end, quiet_end))
                if coordinator.poll_once(drain, timeout):
                    quiet_end = time.monotonic() + 0.05
            return received, active_sends

        received, active_sends = await run(active_deadline)
        if received == 0:
            raise RuntimeError("multi SENDSEND received no active replies")
        schedule = Schedule(time.monotonic(), received, args.duration)
        sampler = LatencySampler()
        paced, _ = await run(
            schedule.epoch + SETTLE_SECONDS + latency_duration(args.duration), schedule, sampler
        )
    if sampler.count == 0:
        raise RuntimeError("multi SENDSEND received no paced replies")
    schedule.print(pattern, args, active_sends, received, paced)
    return result_metrics(
        count=received, msg_size=args.msg_size, elapsed_s=args.duration,
        latency_sampler=sampler, bandwidth_multiplier=2.0,
    )
