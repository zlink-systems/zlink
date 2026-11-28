# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


@pytest.mark.parametrize("topic", ["prefix\x00suffix", b"prefix\x00suffix"])
def test_subscription_operations_reject_an_embedded_nul(topic):
    with zlink.create_context() as context, zlink.create_sub_socket(context) as subscriber:
        subscriber.set_subscription("prefix")
        for operation in (subscriber.set_subscription, subscriber.unset_subscription):
            with pytest.raises(ValueError, match="NUL"):
                operation(topic)
            assert subscriber.sub_options.topics_count == 1
            assert subscriber.subscription_at(0) == ("prefix", False)


@pytest.mark.parametrize("topic", ["prefix\x00suffix", b"prefix\x00suffix"])
def test_publish_rejects_an_embedded_nul(topic):
    with (
        zlink.create_context() as context,
        zlink.create_pub_socket(context) as publisher,
        zlink.Message.from_(b"payload") as message,
    ):
        for payload in (b"payload", message):
            with pytest.raises(ValueError, match="NUL|null|embedded"):
                publisher.publish(topic).message(payload).submit()
