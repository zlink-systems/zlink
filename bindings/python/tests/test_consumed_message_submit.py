# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


def test_consumed_message_cannot_be_submitted_again():
    with (
        zlink.create_context() as context,
        zlink.create_pair_socket(context) as sender,
        zlink.create_pair_socket(context) as receiver,
        zlink.create_received() as received,
        zlink.Message.from_(b"payload") as message,
    ):
        sender.options.linger_ms = receiver.options.linger_ms = 0
        receiver.options.receive_timeout_ms = 1000
        sender.bind("inproc://consumed-message")
        receiver.connect("inproc://consumed-message")

        sender.send().message(message).submit_sync()
        with pytest.raises(zlink.ConfigError) as raised:
            sender.send().message(message).submit_sync()

        assert raised.value.result is zlink.ConfigResult.INVALID_HANDLE
        assert message.size() == 0
        assert bytes(message.data) == b""
        assert receiver.recv_into(received)
        assert received.to_bytes_list() == [b"payload"]
        assert not receiver.recv_into(received, flags=zlink.RecvFlags.DONT_WAIT)
