import queue
import sys
import time
from contextlib import ExitStack

import zlink

from perf_multi_common import (
    HEADER_SIZE,
    TOPIC,
    apply_multi_socket_options,
    benchmark_run_id,
    configure_multi_tls_client,
    LatencySampler,
    parse_client_args,
    perf_client_context,
    resolve_multi_monitor_hwm_bytes,
    print_result_lines,
    print_multi_auto_hwm_detail,
    recv_nonblocking,
    resolve_multi_connect_ready_timeout_ms,
    result_metrics,
    measurement_part_count,
    safe_poll,
    wait_monitor_event,
)

from perf_metrics import interpolated_percentile
from perf_multi_measurement import (
    CatchUp, MAX_BURST_PER_SENDER, NS_PER_SECOND, PHASE_ACTIVE, PHASE_LATENCY,
    PHASE_WARMUP, Schedule, active_receive, latency_duration, metric_header, phase_commands,
)


def main(argv=None):
    args = parse_client_args(
        argv or sys.argv[1:], pattern="pubsub"
    )
    run_id = benchmark_run_id()
    latency_sampler = LatencySampler()
    reference = LatencySampler()

    with perf_client_context() as ctx:
        sockets = [zlink.create_sub_socket(ctx) for _ in range(args.clients)]
        try:
            with ExitStack() as stack:
                monitors = []
                for sock in sockets:
                    apply_multi_socket_options(sock)
                    monitor = stack.enter_context(
                        sock.monitor_open(
                            zlink.MonitorEventMask.CONNECTION_READY,
                            resolve_multi_monitor_hwm_bytes(),
                        )
                    )
                    configure_multi_tls_client(sock, args.transport)
                    sock.set_subscription(TOPIC)
                    sock.connect(args.endpoint)
                    monitors.append(monitor)
                for monitor in monitors:
                    wait_monitor_event(
                        monitor,
                        zlink.MonitorEventMask.CONNECTION_READY,
                        timeout_ms=resolve_multi_connect_ready_timeout_ms(),
                    )
            print(f"CLIENT_READY,{args.msg_size}", flush=True)
            while True:
                command = sys.stdin.readline().strip()
                if command == f"START,{args.msg_size}":
                    break
                raise SystemExit(f"unexpected command: {command}")

            ctx.recalculate_auto_hwm()
            commands = phase_commands()
            active = [0] * len(sockets)
            paced = [0] * len(sockets)
            samples = [0] * len(sockets)
            tracker = CatchUp()
            epoch = None
            caught_up = False
            duration = latency_duration(args.duration)
            inflight_floor = None
            next_progress = 0
            recv_storage = [zlink.create_topic_message() for _ in sockets]

            with zlink.create_poller() as poller:
                events = zlink.create_poll_events(len(sockets))
                for index, sock in enumerate(sockets):
                    poller.add_socket(sock, zlink.PollEventFlag.POLLIN, index)

                def receive_phase(phase, seconds, schedule=None):
                    nonlocal epoch, caught_up, inflight_floor, next_progress
                    deadline = time.monotonic_ns() + int(seconds * NS_PER_SECOND)
                    drain_end = deadline + 10 * NS_PER_SECOND
                    last_payload = time.monotonic()
                    done_at = None
                    done_line = f"PHASE_DONE,{args.msg_size},{phase}"
                    stopped = set()
                    while time.monotonic_ns() < drain_end:
                        try:
                            command, when = commands.get_nowait()
                            if command == done_line:
                                done_at = when
                        except queue.Empty:
                            pass
                        if done_at is not None and (
                            len(stopped) == len(sockets)
                            or time.monotonic() - max(last_payload, done_at) >= 0.05
                        ):
                            return
                        ready = safe_poll(poller, events, 10)
                        for offset in range(ready):
                            index = events.slot(offset)
                            if index >= len(sockets) or not events.revents(offset) & int(zlink.PollEventFlag.POLLIN):
                                continue
                            for _ in range(MAX_BURST_PER_SENDER):
                                received = recv_nonblocking(
                                    sockets[index], method="subscribe", storage=recv_storage[index]
                                )
                                if received is None:
                                    break
                                observed = time.monotonic_ns()
                                last_payload = observed / NS_PER_SECOND
                                with received:
                                    parts = received.parts
                                    if len(parts) != measurement_part_count() or (
                                        len(parts) == 2 and len(parts[1].data) != 0
                                    ):
                                        raise RuntimeError("invalid measured multipart pubsub payload")
                                    data = parts[0].data
                                    wire_size = len(data)
                                    header = metric_header(
                                        data, args.msg_size, run_id, marker=wire_size == HEADER_SIZE
                                    )
                                if header is None:
                                    continue
                                if header["seq"] == 0 and header["sent_ts_ns"] == 0:
                                    if header["phase"] == phase:
                                        stopped.add(index)
                                    continue
                                if wire_size != args.msg_size:
                                    continue
                                if schedule is None:
                                    if header["phase"] == PHASE_ACTIVE and active_receive(observed, deadline):
                                        active[index] += 1
                                    continue
                                if header["phase"] not in (PHASE_WARMUP, PHASE_LATENCY):
                                    continue
                                sent = header["sent_ts_ns"]
                                paced[index] += 1
                                if not 0 < sent <= observed:
                                    continue
                                if epoch is None:
                                    epoch = sent
                                if not caught_up:
                                    reference.add(observed - sent)
                                elif header["phase"] == PHASE_LATENCY:
                                    samples[index] += 1
                                    latency_sampler.add(observed - sent)

                        if schedule is not None and epoch is not None and not caught_up:
                            elapsed = (time.monotonic_ns() - epoch) / NS_PER_SECOND
                            scheduled = schedule.scheduled(elapsed)
                            caught_up = elapsed <= duration and tracker.observe(elapsed, paced, scheduled)
                            if elapsed >= next_progress:
                                floor = max(0, scheduled - min(paced))
                                inflight_floor = floor if inflight_floor is None else min(inflight_floor, floor)
                                print(f"PUBSUB_CATCHUP_PROGRESS,{args.msg_size},{elapsed:.6g},{floor},{min(paced)},{scheduled},{schedule.rate:.6g}", flush=True)
                                next_progress = elapsed + 1
                            if caught_up:
                                print(f"PHASE_CATCHUP,{args.msg_size}", flush=True)
                                print(
                                    f"PUBSUB_CATCHUP_DIAG,{args.msg_size},{elapsed:.6g},{tracker.in_flight}"
                                    f",window_start_in_flight,{tracker.start_in_flight}", flush=True,
                                )
                    raise TimeoutError("multi pubsub receive drain timed out")

                receive_phase(PHASE_ACTIVE, args.duration)
                minimum = min(active)
                if minimum == 0:
                    raise RuntimeError("multi pubsub subscriber received no active messages")
                print(f"PHASE_DRAINED,{args.msg_size},{minimum}", flush=True)
                schedule = Schedule(0, minimum, args.duration)
                receive_phase(PHASE_LATENCY, 2 * duration, schedule)

            print(f"PHASE_COMPLETE,{args.msg_size},{min(samples)}", flush=True)
            metrics = result_metrics(
                count=sum(active), msg_size=args.msg_size, elapsed_s=args.duration,
                latency_sampler=latency_sampler,
            )
            if latency_sampler.count:
                p50 = interpolated_percentile(latency_sampler.samples, 0.5) / 1e6
                print(f"PUBSUB_LATENCY_DIAG,{args.msg_size},latency_p50_ms,{p50:.6f}", flush=True)
            else:
                for name in ("latency", "latency_p95", "latency_p99"):
                    metrics[name] = "invalid:receiver_not_caught_up_by_deadline"
                key = f"REFERENCE,current,PUBSUB,{args.transport},{args.msg_size}"
                floor = inflight_floor or 0
                print(f"{key},inflight_floor,{floor}", flush=True)
                for name, value in (
                    ("latency_ref_p50", interpolated_percentile(reference.samples, 0.5) / 1e6),
                    ("latency_ref_p99", interpolated_percentile(reference.samples, 0.99) / 1e6),
                    ("inflight_floor_ms", floor / schedule.rate * 1000),
                ):
                    print(f"{key},{name},{value:.6f}", flush=True)
            print_multi_auto_hwm_detail(
                sockets[0], "endpoint", args.transport, args.msg_size, "sub"
            )
            print_result_lines("PUBSUB", args.transport, args.msg_size, metrics)
            print(f"CLIENT_DONE,{args.msg_size}", flush=True)
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except Exception as exc:
                    print(f"[perf] close failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    main()
