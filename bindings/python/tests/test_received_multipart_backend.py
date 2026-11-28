# SPDX-License-Identifier: MPL-2.0

import zlink


def test_multipart_receive_wraps_each_part_in_order_and_keeps_its_snapshot():
    with (
        zlink.create_context() as context,
        zlink.create_pair_socket(context) as sender,
        zlink.create_pair_socket(context) as receiver,
        zlink.create_received() as received,
    ):
        sender.options.linger_ms = receiver.options.linger_ms = 0
        receiver.options.receive_timeout_ms = 1000
        sender.bind("inproc://multipart-backend")
        receiver.connect("inproc://multipart-backend")
        sender.send().messages(b"a", b"", b"b").submit_sync()

        assert receiver.recv_into(received)
        views = [part.data for part in received]
        received.close()
        assert [bytes(view) for view in views] == [b"a", b"", b"b"]
