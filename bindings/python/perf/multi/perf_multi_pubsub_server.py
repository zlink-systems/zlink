import queue
import sys
import time

import zlink

from perf_multi_common import (
    HEADER_SIZE,
    TOPIC,
    apply_multi_socket_options,
    benchmark_endpoint,
    benchmark_run_id,
    configure_multi_tls_server,
    new_payload,
    parse_server_args,
    perf_server_context,
    print_multi_auto_hwm_detail,
    publish_sync,
    stamp_payload,
)

from perf_multi_measurement import (
    PHASE_ACTIVE, PHASE_LATENCY, PHASE_WARMUP,
    Schedule, latency_duration, phase_commands, read_phase_count, wait_ms,
)


def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    run_id = benchmark_run_id()
    endpoint = benchmark_endpoint(args.transport, "multi-pubsub")
    payload = new_payload(args.msg_size)
    duration = latency_duration(args.duration)

    with perf_server_context() as ctx:
        with zlink.create_pub_socket(ctx) as publisher:
            configure_multi_tls_server(publisher, args.transport)
            apply_multi_socket_options(publisher)
            publisher.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)
            if sys.stdin.readline().strip() != f"START,{args.msg_size}":
                return
            ctx.recalculate_auto_hwm()

            def publish(phase):
                publish_sync(publisher, TOPIC, stamp_payload(payload, phase=phase, run_id=run_id))

            def done(phase):
                marker = stamp_payload(
                    new_payload(HEADER_SIZE), phase, run_id=run_id,
                    msg_size=args.msg_size, seq=0, sent_ts_ns=0,
                )
                publish_sync(publisher, TOPIC, marker)
                print(f"PHASE_DONE,{args.msg_size},{phase}", flush=True)

            end = time.monotonic() + args.duration
            active_sends = 0
            while time.monotonic() < end:
                publish(PHASE_ACTIVE)
                active_sends += time.monotonic() < end
            print_multi_auto_hwm_detail(publisher, "endpoint", args.transport, args.msg_size, "pub")
            done(PHASE_ACTIVE)
            received = read_phase_count("PHASE_PACED", args.msg_size)
            commands = phase_commands()
            schedule = Schedule(time.monotonic(), received, args.duration)
            catchup_deadline = schedule.epoch + duration
            end = catchup_deadline + duration
            caught_up = None
            while (now := time.monotonic()) < end:
                if caught_up is None:
                    try:
                        command, when = commands.get_nowait()
                        if command == f"PHASE_CATCHUP,{args.msg_size}" and when <= catchup_deadline:
                            caught_up = when - schedule.epoch
                            end = when + duration
                        elif command in {"STOP", "QUIT"}:
                            return
                    except queue.Empty:
                        pass
                if now >= end:
                    break
                if schedule.due(now):
                    due = schedule.reserve()
                    publish(PHASE_WARMUP if caught_up is None else PHASE_LATENCY)
                    schedule.admit(due, time.monotonic())
                elif (timeout := wait_ms(now, min(end, schedule.next()))) > 0:
                    time.sleep(timeout / 1000)

            done(PHASE_LATENCY)
            prefix = f"PHASE_COMPLETE,{args.msg_size},"
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                command, _ = commands.get(timeout=max(0, deadline - time.monotonic()))
                if command.startswith(prefix):
                    schedule.print("PUBSUB", args, active_sends, received, int(command.removeprefix(prefix)), caught_up)
                    if schedule.late:
                        print(
                            f"REFERENCE,current,PUBSUB,{args.transport},{args.msg_size},sender_late,{schedule.late}",
                            flush=True,
                        )
                    return
            raise TimeoutError("missing pubsub completion")


if __name__ == "__main__":
    main()
