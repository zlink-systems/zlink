import zlink
from sample_support import tcp_endpoint, wait_connected


def main():
# --8<-- [start:doc]
    _, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            with zlink.create_dealer_socket(ctx) as dealer:
                with router.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as router_monitor:
                    with dealer.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as dealer_monitor:
                        dealer.set_routing_id(b"CLIENT")
                        router.bind(endpoint)
                        dealer.connect(endpoint)
                        wait_connected(router_monitor, dealer_monitor)

                dealer.send().message(b"ping").submit()
                request = zlink.create_received()
                if not router.recv_into(request):
                    raise AssertionError("expected dealer-router request")
                with request:
                    if request.routing_id != zlink.RoutingId(b"CLIENT"):
                        raise AssertionError(f"unexpected routing id: {request.routing_id!r}")
                    if request.to_bytes_list() != [b"ping"]:
                        raise AssertionError("unexpected dealer-router request payload")
                    request.send().message(b"pong").submit()

                reply = zlink.create_received()
                if not dealer.recv_into(reply):
                    raise AssertionError("expected dealer-router reply")
                with reply:
                    if reply.to_bytes_list() != [b"pong"]:
                        raise AssertionError("unexpected dealer-router reply payload")
                print('[dealer-router/recv] send: "ping" → recv: "pong"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
