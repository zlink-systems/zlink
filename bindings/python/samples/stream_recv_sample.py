import socket

import zlink
from sample_support import tcp_endpoint, wait_socket_monitor_event


def main():
# --8<-- [start:doc]
    port, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            with server.monitor_open(zlink.MonitorEventMask.ACCEPTED) as server_monitor:
                server.bind(endpoint)
                with socket.create_connection(("127.0.0.1", port), timeout=3.0) as client:
                    wait_socket_monitor_event(server_monitor, zlink.MonitorEventMask.ACCEPTED)
                    client.sendall(b"hello-stream")
                    received = zlink.create_received()
                    if not server.recv_into(received):
                        raise AssertionError("expected stream payload")
                    with received:
                        if received.to_bytes_list() != [b"hello-stream"]:
                            raise AssertionError("unexpected stream payload")
                        if not received.routing_id:
                            raise AssertionError("stream sample expected a routing id")
                        server.send(received.routing_id).message(b"hello-stream").submit()
                    reply = client.recv(64)
                    if reply != b"hello-stream":
                        raise AssertionError(f"unexpected stream reply: {reply!r}")
            print('[stream/recv] send: "hello-stream" → recv: "hello-stream"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
