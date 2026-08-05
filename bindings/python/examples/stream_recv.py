import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import socket
import zlink
from sample_common import tcp_endpoint


def main():
    port, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_stream_socket(ctx) as server:
            server.bind(endpoint)
            with socket.create_connection(("127.0.0.1", port), timeout=3.0) as client:
                client.sendall(b"hello-stream")
                received = zlink.create_received()
                if not server.recv_into(received):
                    raise RuntimeError("expected stream payload")
                with received:
                    received.send().message(b"hello-stream").submit()
                echo = client.recv(64).decode("utf-8")
                print(f'[stream/recv] send: "hello-stream" \u2192 recv: "{echo}"')


if __name__ == "__main__":
    main()
