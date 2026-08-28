import os
import sys
import threading
import time

import zlink

from perf_multi_common import (
    STOP_TOKEN,
    TOPIC,
    apply_multi_socket_options,
    benchmark_endpoint,
    benchmark_run_id,
    configure_multi_tls_server,
    new_payload,
    parse_server_args,
    perf_server_context,
    publish_sync,
    stamp_payload,
)


def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    run_id = benchmark_run_id()
    stop_event = threading.Event()
    start_event = threading.Event()
    endpoint = benchmark_endpoint(args.transport, "multi-pubsub")
    payload = new_payload(args.msg_size)
    active_duration_s = max(
        1.0,
        float(os.environ.get("PERF_MULTI_DURATION_SECONDS", "5")),
    )

    def read_commands():
        for line in sys.stdin:
            text = line.strip().upper()
            if text.startswith("START,"):
                start_event.set()
            elif text in {"STOP", "QUIT"}:
                stop_event.set()
                return
        stop_event.set()

    threading.Thread(target=read_commands, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_pub_socket(ctx) as publisher:
            configure_multi_tls_server(publisher, args.transport)
            apply_multi_socket_options(publisher)
            publisher.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)
            # Start gate (single blocking wait, not a polling cadence).
            start_event.wait()
            if stop_event.is_set():
                return
            active_deadline = time.perf_counter() + active_duration_s
            while time.perf_counter() < active_deadline and not stop_event.is_set():
                publish_sync(
                    publisher,
                    TOPIC,
                    stamp_payload(payload, phase=1, run_id=run_id),
                )
            # PERF_MULTI_TEST_POLICY § 1.3.1: signal phase end on the wire.
            for _ in range(64):
                publisher.publish(TOPIC).message(STOP_TOKEN).submit()
            # Stay alive until runner sends STOP so the published stop token
            # has time to flush to all subscribers (linger_ms == 0).
            stop_event.wait()


if __name__ == "__main__":
    main()
