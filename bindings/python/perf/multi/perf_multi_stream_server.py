import sys
import threading
import errno
from collections import deque

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    parse_server_args,
    perf_server_context,
    safe_poll,
    send_nonblocking,
)
from perf_stop_token import STOP_TOKEN


def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-stream")
    stop = threading.Event()
    pending_ready = threading.Event()
    pending = deque()
    pending_lock = threading.Lock()

    def wait_stop():
        for line in sys.stdin:
            if line.strip() in {"STOP", "QUIT"}:
                stop.set()
                return

    threading.Thread(target=wait_stop, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            configure_multi_tls_server(server, args.transport)
            apply_multi_socket_options(server)
            server.options.tcp_no_delay = True
            server.bind(endpoint)
            print(f"READY,{endpoint}", flush=True)

            def packet_handler(routing_id, header, body):
                # C perf_multi_stream_session.hpp stream_packet_handler_
                # callback: the installed packet handler is the measured
                # POLLIN drain surface; queue the framed echo for POLLOUT
                # backpressure draining.
                body_view = body.data
                if len(body_view) == len(STOP_TOKEN) and body_view == STOP_TOKEN:
                    stop.set()
                    return
                frame = build_packet_frame(header.data, body_view)
                with pending_lock:
                    pending.append((bytes(routing_id), frame))
                pending_ready.set()

            server.on_packet(packet_handler)

            def drain_pending():
                while True:
                    with pending_lock:
                        item = pending[0] if pending else None
                    if item is None:
                        return
                    routing_id, frame = item
                    try:
                        sent = send_nonblocking(server, frame, routing_id=routing_id)
                    except zlink.SubmitError as exc:
                        if exc.native_errno not in {
                            errno.EHOSTUNREACH,
                            errno.ENOTCONN,
                        }:
                            raise
                        sent = True
                    if not sent:
                        return
                    with pending_lock:
                        if pending and pending[0] == item:
                            pending.popleft()
                            if not pending:
                                pending_ready.clear()

            # C run_server_event_loop: POLLOUT-driven backpressure drain of
            # the pending deque with the bounded aux poll wait
            # (perf_aux_poll_wait_ms == 100ms); idle poll when empty. The
            # POLLIN data path is the installed packet handler above.
            aux_wait_ms = 100
            with zlink.create_poller() as poller:
                poller.add_socket(server, zlink.PollEventFlag.POLLOUT, 0)
                poll_events = zlink.create_poll_events(1)
                while not stop.is_set():
                    with pending_lock:
                        has_pending = bool(pending)
                        if not has_pending:
                            pending_ready.clear()
                    if has_pending:
                        drain_pending()
                    with pending_lock:
                        has_pending = bool(pending)
                        if not has_pending:
                            pending_ready.clear()
                    if not has_pending:
                        pending_ready.wait(aux_wait_ms / 1000.0)
                        continue
                    ready_count = safe_poll(poller, poll_events, aux_wait_ms)
                    if ready_count:
                        for offset in range(ready_count):
                            if poll_events.revents(offset) & int(
                                zlink.PollEventFlag.POLLOUT
                            ):
                                drain_pending()


def build_packet_frame(header_view, body_view):
    header_size = len(header_view)
    body_size = len(body_view)
    frame = bytearray(6 + header_size + body_size)
    frame[0] = (header_size >> 8) & 0xFF
    frame[1] = header_size & 0xFF
    _store_u32_be(frame, 2, body_size)
    frame[6 : 6 + header_size] = header_view
    frame[6 + header_size :] = body_view
    return frame


def _store_u32_be(frame, offset, value):
    frame[offset] = (value >> 24) & 0xFF
    frame[offset + 1] = (value >> 16) & 0xFF
    frame[offset + 2] = (value >> 8) & 0xFF
    frame[offset + 3] = value & 0xFF


if __name__ == "__main__":
    main()
