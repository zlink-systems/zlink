import struct
import socket
import threading

import zlink
from sample_support import tcp_endpoint, wait_socket_monitor_event


def main():
# --8<-- [start:doc]
    port, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            with server.monitor_open(zlink.MonitorEventMask.ACCEPTED) as server_monitor:
                done = threading.Event()
                observed = {}

                def on_packet(routing_id, header, body):
                    observed["header"] = header.to_bytes()
                    observed["body"] = body.to_bytes()
                    observed["routing_id"] = routing_id
                    done.set()

                server.bind(endpoint)
                server.on_packet(on_packet)
                with socket.create_connection(("127.0.0.1", port), timeout=3.0) as client:
                    wait_socket_monitor_event(server_monitor, zlink.MonitorEventMask.ACCEPTED)
                    client.sendall(struct.pack("!HI", 0, len(b"hello-stream")) + b"hello-stream")
                    if not done.wait(3.0):
                        raise TimeoutError("stream packet callback did not receive a message")
                    if observed["header"] != b"":
                        raise AssertionError("unexpected stream packet header")
                    if observed["body"] != b"hello-stream":
                        raise AssertionError("unexpected stream packet body")
                    if not observed["routing_id"]:
                        raise AssertionError("stream packet callback expected a routing id")
                    server.send(observed["routing_id"]).message(b"hello-stream").submit()
                    reply = client.recv(64)
                    if reply != b"hello-stream":
                        raise AssertionError(f"unexpected stream packet reply: {reply!r}")
            print('[stream/packet-callback] send: "hello-stream" → recv: "hello-stream"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
