# SPDX-License-Identifier: MPL-2.0

import errno

import pytest
import zlink

from zlink._native.ffi import lib


@pytest.mark.parametrize(
    "role", ["pair", "dealer", "router", "pub", "sub", "xpub", "xsub", "stream"]
)
def test_disconnect_rid_without_peer_projects_not_found(role):
    factory = getattr(zlink, f"create_{role}_socket")
    with zlink.create_context() as context, factory(context) as peer:
        with pytest.raises(zlink.ConnectError) as error:
            peer.disconnect_rid(zlink.RoutingId.from_(b"none"))
        assert error.value.result is zlink.ConnectResult.NOT_FOUND
        assert error.value.code == 605
        assert error.value.internal_errno == errno.ENOENT


def test_stream_disconnect_rid_requires_four_bytes():
    with zlink.create_context() as context, zlink.create_stream_socket(context) as stream:
        with pytest.raises(zlink.ConnectError) as error:
            stream.disconnect_rid(zlink.RoutingId.from_(b"bad"))
        assert error.value.result is zlink.ConnectResult.INVALID_ARGUMENT
        assert error.value.internal_errno == errno.EINVAL


def test_dealer_disconnect_rid_rejects_ambiguous_live_peers():
    rid = zlink.RoutingId.from_(b"duplicate")
    with (
        zlink.create_context() as context,
        zlink.create_dealer_socket(context) as dealer,
        zlink.create_router_socket(context) as first,
        zlink.create_router_socket(context) as second,
        zlink.create_received() as received,
    ):
        dealer.options.receive_timeout_ms = 1000
        for index, router in enumerate((first, second)):
            endpoint = f"inproc://disconnect-conflict-{index}"
            router.set_routing_id(rid)
            router.options.receive_timeout_ms = 1000
            router.bind(endpoint)
            dealer.connect(endpoint)
        # Round-robin one record onto each already attached pipe.
        for _ in (first, second):
            dealer.send().message(b"handshake").submit_sync()
        for index, router in enumerate((first, second)):
            assert router.recv_into(received)
            router.send(received.routing_id).message(bytes([index])).submit_sync()
            assert dealer.recv_into(received)
            assert received.to_bytes_list() == [bytes([index])]

        with pytest.raises(zlink.ConnectError) as error:
            dealer.disconnect_rid(rid)
        assert error.value.result is zlink.ConnectResult.CONFLICT
        assert error.value.code == 606
        assert error.value.internal_errno == errno.EADDRINUSE


def test_disconnect_rid_projects_core_lifecycle_busy(monkeypatch):
    # Force the native result at the FFI boundary without racing Core's lifecycle owner.
    native = lib()
    monkeypatch.setattr(native, "zlink_disconnect_rid", lambda *_: 607)
    monkeypatch.setattr(native, "zlink_errno", lambda: errno.EBUSY)
    with zlink.create_context() as context, zlink.create_router_socket(context) as router:
        with pytest.raises(zlink.ConnectError) as error:
            router.disconnect_rid(zlink.RoutingId.from_(b"peer"))
        assert error.value.result is zlink.ConnectResult.BUSY
        assert error.value.code == 607
        assert error.value.internal_errno == errno.EBUSY
