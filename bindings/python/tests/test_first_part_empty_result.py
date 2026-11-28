# SPDX-License-Identifier: MPL-2.0

import errno

import pytest
import zlink
from zlink._runtime.messaging.message_materializer import _BaseReceived


def test_first_part_of_empty_envelope_is_invalid_state():
    # bindings/doc/spec/README.ko.md Received table: empty first_part() is INVALID_STATE (EINVAL).
    empty = _BaseReceived.__new__(_BaseReceived)
    empty.parts = ()
    with pytest.raises(zlink.RecvError) as raised:
        empty.first_part()
    assert raised.value.result is zlink.RecvResult.INVALID_STATE
    assert raised.value.internal_errno == errno.EINVAL
