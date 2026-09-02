import asyncio
import sys
import threading
import errno
import time

import zlink

from perf_multi_common import (
    apply_multi_socket_options,
    benchmark_endpoint,
    configure_multi_tls_server,
    parse_server_args,
    perf_server_context,
    resolve_multi_monitor_hwm_bytes,
    resolve_multi_connect_ready_timeout_ms,
    send_routed,
    wait_monitor_event,
)
from perf_stop_token import STOP_TOKEN


def classify_control_line(line, msg_size):
    text = line.strip()
    if text in {"STOP", "QUIT"}:
        return "stop"
    if text == f"START,{msg_size}":
        return "start"
    return None


def wait_connection_ready_count(monitor, expected, timeout_ms):
    deadline = time.monotonic() + timeout_ms / 1000.0
    for _ in range(expected):
        remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"timed out waiting for {expected} STREAM connections"
            )
        wait_monitor_event(
            monitor,
            zlink.MonitorEventMask.CONNECTION_READY,
            timeout_ms=remaining_ms,
        )


async def main(argv=None):
    args = parse_server_args(argv or sys.argv[1:])
    endpoint = benchmark_endpoint(args.transport, "multi-stream")
    pending = set()
    send_errors = []
    loop = asyncio.get_running_loop()
    async_stop = asyncio.Event()
    start_requested = asyncio.Event()

    def wait_control():
        for line in sys.stdin:
            command = classify_control_line(line, args.msg_size)
            if command == "start":
                loop.call_soon_threadsafe(start_requested.set)
            elif command == "stop":
                loop.call_soon_threadsafe(async_stop.set)
                return

    threading.Thread(target=wait_control, daemon=True).start()

    with perf_server_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            configure_multi_tls_server(server, args.transport)
            apply_multi_socket_options(server)
            server.options.tcp_no_delay = True
            server.stream_options.recv_mode = zlink.StreamRecvMode.PACKET
            monitor = server.monitor_open(
                zlink.MonitorEventMask.CONNECTION_READY,
                resolve_multi_monitor_hwm_bytes(),
            )
            server.bind(endpoint)

            def packet_handler(routing_id, header, body):
                body_view = body.data
                if len(body_view) == len(STOP_TOKEN) and body_view == STOP_TOKEN:
                    loop.call_soon_threadsafe(async_stop.set)
                    return
                frame = build_packet_frame(header.data, body_view)
                loop.call_soon_threadsafe(schedule_echo, bytes(routing_id), frame)

            async def send_echo(routing_id, frame):
                try:
                    await send_routed(
                        server,
                        frame,
                        routing_id=routing_id,
                        measurement=False,
                        method="send",
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

            print(f"READY,{endpoint}", flush=True)

            start_wait = asyncio.create_task(start_requested.wait())
            stop_wait = asyncio.create_task(async_stop.wait())
            done, pending_control = await asyncio.wait(
                {start_wait, stop_wait}, return_when=asyncio.FIRST_COMPLETED
            )
            for task in pending_control:
                task.cancel()
            await asyncio.gather(*pending_control, return_exceptions=True)
            if stop_wait in done:
                monitor.close()
                return

            # The runner only releases this owner-thread recalculation after the
            # shared raw client reports that every requested connection is ready.
            # Confirm the target-side monitor count before recalculating HWM.
            try:
                wait_connection_ready_count(
                    monitor,
                    args.clients,
                    resolve_multi_connect_ready_timeout_ms(),
                )
                ctx.recalculate_auto_hwm()
                monitor.status()
            finally:
                monitor.close()
            print(f"SERVER_START_READY,{args.msg_size}", flush=True)

            packet = zlink.StreamPacket()
            try:
                while not async_stop.is_set():
                    if server.recv_packet_into(
                        packet, flags=zlink.RecvFlags.DONT_WAIT
                    ):
                        packet_handler(packet.routing_id, packet.header, packet.body)
                        packet.close()
                    else:
                        await asyncio.sleep(0)
            finally:
                packet.close()
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
