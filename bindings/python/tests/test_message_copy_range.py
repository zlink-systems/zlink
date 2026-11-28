# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


def test_copy_to_respects_source_and_destination_byte_offsets():
    with zlink.Message.from_(b"abcdef") as message:
        destination = bytearray(b".......")
        assert message.copy_to(destination, source_offset=1, destination_offset=2, length=3) == 3
        assert destination == b"..bcd.."
        assert message.copy_to(destination, source_offset=4, destination_offset=5) == 2
        assert destination == b"..bcdef"
        assert message.copy_to(destination, source_offset=6, destination_offset=7) == 0
        assert destination == b"..bcdef"


@pytest.mark.parametrize(
    "source_offset,destination_offset,length",
    [(-1, 0, 1), (0, -1, 1), (0, 0, -1), (5, 0, 2), (0, 5, 2), (7, 0, 0), (0, 7, 0)],
)
def test_copy_to_rejects_out_of_bounds_without_writing(source_offset, destination_offset, length):
    with zlink.Message.from_(b"abcdef") as message:
        destination = bytearray(b"......")
        with pytest.raises(ValueError):
            message.copy_to(destination, source_offset, destination_offset, length)
        assert destination == b"......"
