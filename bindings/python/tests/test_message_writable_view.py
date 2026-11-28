# SPDX-License-Identifier: MPL-2.0

import zlink


def test_allocated_message_data_views_writes_and_copy_storage():
    with zlink.Message.allocate(3) as message:
        view = message.data
        assert view is message.data
        view[:] = b"a\0b"
        with message.copy() as copied:
            view[0] = ord("c")
            assert message.to_bytes() == b"c\0b"
            assert copied.to_bytes() == b"a\0b"
