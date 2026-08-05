import zlink
from sample_support import tcp_endpoint, wait_connected


def main():
# --8<-- [start:doc]
    _, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_pair_socket(ctx) as server:
            with zlink.create_pair_socket(ctx) as client:
                with server.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as server_monitor:
                    with client.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as client_monitor:
                        server.bind(endpoint)
                        client.connect(endpoint)
                        wait_connected(server_monitor, client_monitor)

                client.send().message(b"hello-pair").submit()
                received = zlink.create_received()
                if not server.recv_into(received):
                    raise AssertionError("expected pair payload")
                with received:
                    payload = received.to_bytes_list()
                    if payload != [b"hello-pair"]:
                        raise AssertionError(f"unexpected pair payload: {payload!r}")
                print('[pair/recv] send: "hello-pair" → recv: "hello-pair"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
