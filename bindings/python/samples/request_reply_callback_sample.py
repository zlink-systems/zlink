import queue
import threading

import zlink

from sample_support import tcp_endpoint, wait_connected


def main():
# --8<-- [start:doc]
    _, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router_socket:
            with zlink.create_dealer_socket(ctx) as dealer_socket:
                try:
                    with router_socket.monitor_open(
                        zlink.MonitorEventMask.CONNECTION_READY
                    ) as router_monitor:
                        with dealer_socket.monitor_open(
                            zlink.MonitorEventMask.CONNECTION_READY
                        ) as dealer_monitor:
                            dealer_socket.set_routing_id(b"REQ-CLIENT")
                            router_socket.bind(endpoint)
                            dealer_socket.connect(endpoint)
                            wait_connected(router_monitor, dealer_monitor)

                    reply_queue = queue.Queue()
                    dealer_socket.request().message(b"ping").timeout(2.0).submit(
                        lambda result, parts: reply_queue.put((result, parts))
                    )

                    def respond():
                        received = zlink.create_received()
                        if not router_socket.recv_into(received):
                            raise AssertionError("expected request payload")
                        with received:
                            if received.routing_id != zlink.RoutingId(b"REQ-CLIENT"):
                                raise AssertionError("unexpected request routing id")
                            if received.request_seq is None:
                                raise AssertionError("missing request sequence")
                            router_socket.reply(received.routing_id, received.request_seq).message(b"pong").submit()

                    responder = threading.Thread(target=respond)
                    responder.start()
                    result, reply = reply_queue.get(timeout=2.0)
                    responder.join(timeout=2.0)
                    if result != zlink.RequestResult.OK:
                        raise AssertionError(f"request failed: {result!r}")
                    try:
                        if [part.to_bytes() for part in reply] != [b"pong"]:
                            raise AssertionError("unexpected reply payload")
                    finally:
                        for part in reply:
                            part.close()
                    print('[dealer-router/request-reply/callback] send: "ping" -> recv: "pong"')
                finally:
                    pass
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
