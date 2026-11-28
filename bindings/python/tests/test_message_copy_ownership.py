# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


@pytest.mark.parametrize("payload", [b"", b"x" * 4096])
def test_message_copy_has_independent_storage_and_lifetime(payload):
    with zlink.Message.from_(payload) as original:
        with original.copy() as copied:
            if payload:
                with copied.data as writable:
                    writable[0] = ord("y")
                assert copied.to_bytes() == b"y" + payload[1:]
            assert original.to_bytes() == payload
        assert original.to_bytes() == payload

        with original.copy() as copied:
            original.close()
            assert copied.to_bytes() == payload
