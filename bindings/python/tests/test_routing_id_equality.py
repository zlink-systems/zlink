# SPDX-License-Identifier: MPL-2.0

import zlink


def test_routing_id_does_not_equal_int_or_str():
    rid = zlink.RoutingId(bytes(5))
    assert rid != 5
    assert rid != "\x00\x00\x00\x00\x00"
    assert hash(rid) == hash(bytes(5))


def test_routing_id_equals_bytes_like_and_routing_id():
    rid = zlink.RoutingId(b"peer")
    assert rid == b"peer"
    assert rid == bytearray(b"peer")
    assert rid == zlink.RoutingId(b"peer")
    assert rid != b"other"
    assert rid != None  # noqa: E711
