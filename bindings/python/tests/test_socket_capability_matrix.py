# SPDX-License-Identifier: MPL-2.0

from inspect import signature

import pytest
import zlink


@pytest.mark.parametrize(
    "role,operations,role_options",
    [
        ("pair", {"send", "recv_into"}, None),
        ("dealer", {"send", "request", "recv_into"}, "dealer_options"),
        ("router", {"send", "request", "reply", "recv_into"}, "router_options"),
        ("pub", {"publish"}, "pub_options"),
        ("sub", {"subscribe_into", "set_subscription", "unset_subscription", "subscription_at"}, "sub_options"),
        ("xpub", {"publish", "receive_subscription_event_into"}, "pub_options"),
        ("xsub", {"subscribe_into", "set_subscription", "unset_subscription", "subscription_at"}, "sub_options"),
        ("stream", {"send", "recv_into", "recv_packet_into"}, "stream_options"),
    ],
)
def test_public_socket_roles_match_the_capability_matrix(role, operations, role_options):
    data_operations = {
        "send", "request", "reply", "recv_into", "publish", "subscribe_into",
        "receive_subscription_event_into", "recv_packet_into",
        "set_subscription", "unset_subscription", "subscription_at",
    }
    option_roles = {
        "dealer_options", "router_options", "pub_options", "sub_options",
        "stream_options",
    }
    factory = getattr(zlink, f"create_{role}_socket")
    with zlink.create_context() as context, factory(context) as peer:
        for surface in (type(peer), peer):
            assert {name for name in data_operations if hasattr(surface, name)} == operations
            assert {name for name in option_roles if hasattr(surface, name)} == (
                {role_options} if role_options else set()
            )
            assert hasattr(surface, "options")
            for name in ("bind", "unbind", "disconnect_rid", "set_tls_server", "set_tls_client"):
                assert callable(getattr(surface, name))
            for name in ("connect", "disconnect", "connection_attempts_snapshot"):
                assert hasattr(surface, name) is (role != "stream")
            for name in ("set_routing_id", "get_routing_id"):
                assert hasattr(surface, name) is (role in {"dealer", "router", "stream"})
            for name in ("set_packet_handler", "on_packet", "on_receive", "on_subscribe"):
                assert not hasattr(surface, name)

        if "send" in operations:
            rid = zlink.RoutingId.from_(b"target")
            args = (rid,) if role in {"router", "stream"} else ()
            signature(peer.send).bind(*args)
            with pytest.raises(TypeError):
                signature(peer.send).bind(*((rid,) if not args else ()))

        for surface in (peer, peer.options, context, context.options):
            for name in (
                "get_option", "set_option", "get_sockopt", "set_sockopt",
                "getsockopt", "setsockopt",
            ):
                assert not hasattr(surface, name)
