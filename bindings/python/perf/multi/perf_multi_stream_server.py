import asyncio
import sys
import threading
import errno

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    parse_server_args,
    perf_server_context,
    send_routed,
)
from perf_stop_token import STOP_TOKEN


async def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-stream")
    pending = set()
    send_errors = []
    loop = asyncio.get_running_loop()
    async_stop = asyncio.Event()

    def request_stop():
        loop.call_soon_threadsafe(async_stop.set)

    def wait_stop():
        for line in sys.stdin:
            if line.strip() in {"STOP", "QUIT"}:
                request_stop()
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
                body_view = body.data
                if len(body_view) == len(STOP_TOKEN) and body_view == STOP_TOKEN:
                    request_stop()
                    return
                frame = build_packet_frame(header.data, body_view)
                loop.call_soon_threadsafe(schedule_echo, bytes(routing_id), frame)

            server.on_packet(packet_handler)

            async def send_echo(routing_id, frame):
                try:
                    await send_routed(
                        server,
                        frame,
                        routing_id=routing_id,
                        measurement=False,
                        method="send_async",
                    )
                except zlink.SubmitError as exc:
                    if exc.result not in {
                        zlink.SubmitResult.NOT_CONNECTED,
                        zlink.SubmitResult.NOT_FOUND,
                    } and exc.native_errno not in {errno.EHOSTUNREACH, errno.ENOTCONN}:
                        raise

            def on_send_done(task):
                pending.discard(task)
                if not task.cancelled() and task.exception() is not None:
                    send_errors.append(task.exception())

            def schedule_echo(routing_id, frame):
                task = asyncio.create_task(send_echo(routing_id, frame))
                pending.add(task)
                task.add_done_callback(on_send_done)

            # Packet callbacks may arrive from the binding dispatcher thread;
            # schedule every public async send onto this coroutine loop.  The
            # cross-thread stop event is signal-driven, not a timer pump.
            await async_stop.wait()
            if send_errors:
                raise send_errors[0]
            if pending:
                await asyncio.gather(*pending)


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
    asyncio.run(main())
