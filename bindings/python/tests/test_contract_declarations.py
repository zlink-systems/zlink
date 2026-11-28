# SPDX-License-Identifier: MPL-2.0

"""Contract declarations that must match what the runtime exposes."""

import typing

import zlink


def test_topic_message_contract_declares_routing_id():
    assert "routing_id" in typing.get_type_hints(zlink.TopicMessage)
    assert isinstance(zlink.create_topic_message(), zlink.TopicMessage)
    assert zlink.create_topic_message().routing_id is None


def test_weight_docs_state_core_range():
    for cls in (zlink.DealerSocketOptions, zlink.RouterSocketOptions):
        assert "0..10000" in cls.__doc__
        assert "0-100" not in cls.__doc__
