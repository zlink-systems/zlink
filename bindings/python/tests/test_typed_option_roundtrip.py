# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


@pytest.mark.parametrize(
    "factory,role,settings",
    [
        (zlink.create_router_socket, "options", {
            "linger_ms": 57,
            "send_high_water_mark": 2**32 + 13,
            "receive_high_water_mark": 2**32 + 29,
            "send_timeout_ms": 211,
            "receive_timeout_ms": 307,
            "immediate": True,
            "rid_duplicate_policy": zlink.RidDuplicatePolicy.HANDOVER,
            "connect_timeout_ms": 407,
            "ipv6": True,
            "tcp_no_delay": False,
            "tcp_keepalive": 1,
            "max_message_size": 2**33 + 41,
            "backlog": 61,
            "reconnect_interval_ms": 107,
            "reconnect_interval_max_ms": 809,
            "submit_retry_mode": zlink.SubmitRetryMode.LOCAL_FAILURE,
            "submit_retry_timeout_ms": 251,
            "submit_retry_attempts": 11,
        }),
        (zlink.create_router_socket, "router_options", {
            "mandatory": True, "handover": True, "probe": True,
            "connect_routing_id": zlink.RoutingId.from_(b"outbound\x00\xff"),
            "weight": 701, "request_timeout_ms": 359,
        }),
        (zlink.create_dealer_socket, "dealer_options", {
            "probe": True, "weight": 541, "request_timeout_ms": 463,
        }),
        (zlink.create_stream_socket, "stream_options", {
            "notify": True, "recv_mode": zlink.StreamRecvMode.RAW,
        }),
        (zlink.create_xpub_socket, "pub_options", {
            "verbose": True, "verboser": True, "manual": True, "no_drop": True,
            "manual_last_value": True,
        }),
        (zlink.create_pub_socket, "pub_options", {
            "verbose": True, "verboser": True, "manual": True, "no_drop": True,
            "manual_last_value": True,
        }),
    ],
    ids=["common", "router", "dealer", "stream", "xpub", "pub"],
)
def test_socket_option_round_trip(factory, role, settings):
    with zlink.create_context() as context, factory(context) as peer:
        options = getattr(peer, role)
        for name, value in settings.items():
            setattr(options, name, value)
        for name, expected in settings.items():
            actual = getattr(options, name)
            assert type(actual) is type(expected), name
            assert actual == expected, name


def test_context_option_round_trip():
    with zlink.create_context() as context:
        options = context.options
        settings = {
            "io_threads": 2, "max_sockets": 256, "thread_scheduling_policy": 0,
            "thread_name_prefix": "py-options", "auto_hwm_enabled": False,
            "auto_hwm_recalc_debounce": 511, "blocky": False,
            "core_hwm_profile": zlink.AutoHwmProfile.THROUGHPUT,
            "core_hwm_memory_limit_bytes": 32 * 1024 * 1024,
            "core_hwm_budget_bytes": 8 * 1024 * 1024,
        }
        for name, value in settings.items():
            setattr(options, name, value)
        for name, expected in settings.items():
            actual = getattr(options, name)
            assert type(actual) is type(expected), name
            assert actual == expected, name
        options.thread_name_prefix = "é" * 7 + "x"
        assert options.thread_name_prefix == "é" * 7 + "x"
        with pytest.raises(zlink.ConfigError) as error:
            options.thread_name_prefix = "é" * 8
        assert error.value.result is zlink.ConfigResult.INVALID_ARGUMENT
        assert options.thread_name_prefix == "é" * 7 + "x"
        options.thread_name_prefix = ""
        assert options.thread_name_prefix == ""
        for name in ("socket_limit", "msg_t_size"):
            assert getattr(options, name) > 0
            with pytest.raises(AttributeError):
                setattr(options, name, 0)


@pytest.mark.parametrize("factory", [zlink.create_pub_socket, zlink.create_xpub_socket])
def test_publisher_message_option_round_trip(factory):
    with zlink.create_context() as context, factory(context) as publisher:
        with zlink.Message.from_(b"welcome") as source:
            publisher.pub_options.welcome_message = source
        with publisher.pub_options.welcome_message as message:
            assert message.to_bytes() == b"welcome"
        assert publisher.pub_options.topics_count == 0
        with pytest.raises(AttributeError):
            publisher.pub_options.topics_count = 1


@pytest.mark.parametrize("factory", [zlink.create_sub_socket, zlink.create_xsub_socket])
def test_subscriber_topics_count_is_read_only(factory):
    with zlink.create_context() as context, factory(context) as subscriber:
        assert subscriber.sub_options.topics_count == 0
        subscriber.set_subscription("topic")
        assert subscriber.sub_options.topics_count == 1
        with pytest.raises(AttributeError):
            subscriber.sub_options.topics_count = 0
        subscriber.unset_subscription("topic")
        assert subscriber.sub_options.topics_count == 0

def test_raw_socket_options_do_not_expose_removed_heartbeat_controls():
    with zlink.create_context() as context, zlink.create_router_socket(context) as peer:
        for name in ("heartbeat_interval_ms", "heartbeat_ttl_ms", "heartbeat_timeout_ms"):
            assert not hasattr(type(peer.options), name)
