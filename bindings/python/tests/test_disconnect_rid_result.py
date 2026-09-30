# SPDX-License-Identifier: MPL-2.0

import socket
import struct
import uuid

import pytest
import zlink


def test_disconnect_rid_not_found_uses_connect_error_after_peer_disconnect():
    endpoint = f"inproc://disconnect-rid-{uuid.uuid4().hex}"
    with zlink.create_context() as ctx:
        with zlink.create_router_socket(ctx) as router, zlink.create_dealer_socket(
            ctx
        ) as dealer:
            rid = zlink.RoutingId.from_(f"disconnect-rid-{uuid.uuid4().hex}")
            dealer.set_routing_id(rid)
            router.bind(endpoint)
            dealer.connect(endpoint)

            with dealer.monitor_open(
                zlink.MonitorEventMask.DISCONNECTED
            ) as monitor, zlink.create_poller() as poller:
                poller.add_socket(monitor, zlink.PollEventFlag.POLLIN, 1)
                dealer.send().message(b"ready").submit_sync()
                received = zlink.create_received()
                try:
                    assert router.recv_into(received)
                    assert received.routing_id == rid
                    router.disconnect_rid(rid)

                    events = zlink.create_poll_events(1)
                    assert poller.wait(events, 5000) == 1
                    event = monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)
                    assert event is not None
                    assert event.event == zlink.MonitorEventMask.DISCONNECTED

                    with pytest.raises(zlink.ConnectError) as error:
                        router.disconnect_rid(rid)
                    assert error.value.result == zlink.ConnectResult.NOT_FOUND
                    assert int(error.value.result) == 605
                finally:
                    received.close()


def test_stream_disconnect_rid_closes_client_and_returns_connect_not_found():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    endpoint = f"tcp://127.0.0.1:{port}"
    payload = b"disconnect-peer"
    frame = struct.pack("!HI", 0, len(payload)) + payload

    with zlink.create_context() as ctx:
        with zlink.create_stream_socket(ctx) as stream:
            stream.stream_options.recv_mode = zlink.StreamRecvMode.PACKET
            stream.bind(endpoint)
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.settimeout(5)
                client.sendall(frame)
                with zlink.StreamPacket() as packet:
                    assert stream.recv_packet_into(packet)
                    peer_rid = packet.routing_id
                    assert peer_rid is not None

                    stream.disconnect_rid(peer_rid)
                    assert client.recv(1) == b""

                    with pytest.raises(zlink.ConnectError) as error:
                        stream.disconnect_rid(peer_rid)
                    assert error.value.result == zlink.ConnectResult.NOT_FOUND
                    assert int(error.value.result) == 605
