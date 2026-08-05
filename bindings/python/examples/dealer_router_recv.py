import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import zlink
from sample_common import tcp_endpoint, wait_connected


def main():
    port, endpoint = tcp_endpoint()
    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router:
            with zlink.create_dealer_socket(ctx) as dealer:
                with router.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as rtr_mon:
                    with dealer.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as dlr_mon:
                        dealer.set_routing_id(b"CLIENT")
                        router.bind(endpoint)
                        dealer.connect(endpoint)
                        wait_connected(rtr_mon, dlr_mon)

                dealer.send().message(b"ping").submit()
                request = zlink.create_received()
                if not router.recv_into(request):
                    raise RuntimeError("expected dealer-router request")
                with request:
                    router.send(request.routing_id).message(b"pong").submit()

                response = zlink.create_received()
                if not dealer.recv_into(response):
                    raise RuntimeError("expected dealer-router response")
                with response:
                    data = response.to_bytes_list()[0].decode("utf-8")
                    print(f'[dealer-router/recv] send: "ping" \u2192 recv: "{data}"')


if __name__ == "__main__":
    main()
