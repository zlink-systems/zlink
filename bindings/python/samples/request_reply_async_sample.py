import asyncio

import zlink

from sample_support import tcp_endpoint, wait_connected


def respond(router_socket):
    received = zlink.create_received()
    if not router_socket.recv_into(received):
        raise AssertionError("expected request payload")
    with received:
        if received.routing_id != zlink.RoutingId(b"REQ-CLIENT"):
            raise AssertionError("unexpected request routing id")
        if received.reply_token is None:
            raise AssertionError("missing reply token")
        router_socket.reply(received.routing_id, received.reply_token).message(
            b"pong"
        ).submit()


async def main():
# --8<-- [start:doc]
    _, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router_socket:
            with zlink.create_dealer_socket(ctx) as dealer_socket:
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

                pending_reply = asyncio.create_task(
                    dealer_socket.request().message(b"ping").timeout(2.0).submit()
                )
                await asyncio.to_thread(respond, router_socket)
                reply = await pending_reply
                try:
                    if [part.to_bytes() for part in reply] != [b"pong"]:
                        raise AssertionError("unexpected reply payload")
                finally:
                    for part in reply:
                        part.close()
                print('[dealer-router/request-reply/async] send: "ping" -> recv: "pong"')
# --8<-- [end:doc]


if __name__ == "__main__":
    asyncio.run(main())
