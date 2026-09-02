import sys
import threading

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    measurement_part_count,
    parse_server_args,
    PERF_MULTI_AUX_POLL_WAIT_MS,
    perf_server_context,
    recv_nonblocking,
    safe_poll,
)


def submit_reqrep_reply(request, parts):
    """Submit one HWM-free raw reply, draining only a vanished route.

    Core owns multipart staging and rollback. Its raw reply terminal cannot
    report admission backpressure or an admission timeout, so retrying a
    public builder here would invent a readiness/retry contract that raw reply
    does not have. A peer disappearing before commit is an expected terminal
    drain; every other result is a benchmark failure.
    """

    try:
        request.reply().messages(*parts).submit()
        return True
    except zlink.SubmitError as exc:
        if exc.result in {
            zlink.SubmitResult.NOT_CONNECTED,
            zlink.SubmitResult.NOT_FOUND,
        }:
            return False
        raise


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
                    ready_count = safe_poll(
                        poller, poll_events, PERF_MULTI_AUX_POLL_WAIT_MS
                    )
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
                                # The binding materializer clones native-backed
                                # ReceivedMessage parts by zlink_msg reference;
                                # raw reply submission is synchronous, so the
                                # envelope remains valid until ownership has
                                # transferred and no payload snapshot is needed.
                                parts = request.parts
                                if len(parts) != measurement_part_count():
                                    raise RuntimeError(
                                        "invalid measured multipart request"
                                    )
                                if len(parts) == 2 and len(parts[1]) != 0:
                                    raise RuntimeError(
                                        "invalid measured multipart trailing frame: "
                                        f"sizes={[len(part) for part in parts]}"
                                    )
                                if (
                                    request.routing_id is None
                                    or request.reply_token is None
                                ):
                                    raise RuntimeError(
                                        "request is missing routing correlation metadata"
                                    )
                                submit_reqrep_reply(request, parts)


__all__ = ["run_reqrep_server"]
