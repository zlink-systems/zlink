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
    safe_poll,
)


def run_reqrep_server(argv, *, endpoint_token, routed_server):
    args = parse_server_args(argv)
    endpoint = benchmark_endpoint(args.transport, endpoint_token)
    stop = threading.Event()

    def wait_stop():
        for line in sys.stdin:
            if line.strip().upper() in {"STOP", "QUIT"}:
                stop.set()
                return
        stop.set()

    threading.Thread(target=wait_stop, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            if routed_server:
                router.set_routing_id(b"SERVER")
            configure_multi_tls_server(router, args.transport)
            apply_multi_socket_options(router)
            router.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)

            with zlink.create_poller() as poller:
                poller.add_socket(router, zlink.PollEventFlag.POLLIN, 0)
                poll_events = zlink.create_poll_events(1)
                received = zlink.create_received()
                while not stop.is_set():
                    ready_count = safe_poll(poller, poll_events, 5)
                    for offset in range(ready_count):
                        if poll_events.slot(offset) != 0 or not (
                            poll_events.revents(offset)
                            & int(zlink.PollEventFlag.POLLIN)
                        ):
                            continue
                        while not stop.is_set():
                            request = recv_nonblocking(router, storage=received)
                            if request is None:
                                break
                            with request:
                                parts = request.to_bytes_list()
                                if len(parts) != measurement_part_count():
                                    raise RuntimeError(
                                        "invalid measured multipart request"
                                    )
                                if len(parts) == 2 and parts[1] != b"":
                                    raise RuntimeError(
                                        "invalid measured multipart trailing frame"
                                    )
                                if (
                                    request.routing_id is None
                                    or request.request_seq is None
                                ):
                                    raise RuntimeError(
                                        "request is missing routing correlation metadata"
                                    )
                                try:
                                    # Raw request reply is sync-only in the
                                    # public Python contract. The blocking
                                    # terminal owns Core/HWM admission.
                                    request.reply().messages(*parts).submit()
                                except zlink.SubmitError as exc:
                                    if exc.result not in {
                                        zlink.SubmitResult.NOT_CONNECTED,
                                        zlink.SubmitResult.NOT_FOUND,
                                    }:
                                        raise


__all__ = ["run_reqrep_server"]
