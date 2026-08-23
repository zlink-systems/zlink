import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import asyncio
import zlink
from sample_common import tcp_endpoint, wait_connected


def main():
    port, endpoint = tcp_endpoint()
    with zlink.create_context() as ctx:
        with zlink.create_pair_socket(ctx) as server:
            with zlink.create_pair_socket(ctx) as client:
                with server.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as srv_mon:
                    with client.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as cli_mon:
                        server.bind(endpoint)
                        client.connect(endpoint)
                        wait_connected(srv_mon, cli_mon)

                asyncio.run(client.send().message(b"hello-pair").submit())
                received = zlink.create_received()
                if not server.recv_into(received):
                    raise RuntimeError("expected pair payload")
                with received:
                    data = received.to_bytes_list()[0].decode("utf-8")
                print(f'[pair/recv] send: "hello-pair" \u2192 recv: "{data}"')


if __name__ == "__main__":
    main()
