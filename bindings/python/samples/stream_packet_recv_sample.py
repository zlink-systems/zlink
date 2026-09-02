import socket
import struct

import zlink
from sample_support import tcp_endpoint, wait_socket_monitor_event


def main():
# --8<-- [start:doc]
    port, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            server.stream_options.recv_mode = zlink.StreamRecvMode.PACKET
            with server.monitor_open(zlink.MonitorEventMask.ACCEPTED) as server_monitor:
                server.bind(endpoint)
                with socket.create_connection(("127.0.0.1", port), timeout=3.0) as client:
                    wait_socket_monitor_event(server_monitor, zlink.MonitorEventMask.ACCEPTED)
                    client.sendall(
                        struct.pack("!HI", 0, len(b"hello-stream")) + b"hello-stream"
                    )
                    packet = zlink.StreamPacket()
                    if not server.recv_packet_into(packet):
                        raise AssertionError("expected stream packet")
                    with packet:
                        if packet.header.to_bytes() != b"":
                            raise AssertionError("unexpected stream packet header")
                        if packet.body.to_bytes() != b"hello-stream":
                            raise AssertionError("unexpected stream packet body")
                        if not packet.routing_id:
                            raise AssertionError("stream packet expected a routing id")
                        server.send(packet.routing_id).message(b"hello-stream").submit_sync()
                    reply = client.recv(64)
                    if reply != b"hello-stream":
                        raise AssertionError(f"unexpected stream packet reply: {reply!r}")
            print('[stream/packet-recv] send: "hello-stream" -> recv: "hello-stream"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
