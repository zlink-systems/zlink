# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


@pytest.mark.parametrize("invalid", [-1, 3, 1 << 32])
def test_stream_receive_mode_rejects_out_of_range_enum_values(invalid):
    with zlink.create_context() as context, zlink.create_stream_socket(context) as stream:
        options = stream.stream_options
        options.recv_mode = zlink.StreamRecvMode.RAW
        with pytest.raises(ValueError):
            options.recv_mode = invalid
        assert options.recv_mode is zlink.StreamRecvMode.RAW


@pytest.mark.parametrize("invalid", [-1, 2, 1 << 32])
def test_rid_duplicate_policy_rejects_out_of_range_enum_values(invalid):
    with zlink.create_context() as context, zlink.create_router_socket(context) as router:
        options = router.options
        options.rid_duplicate_policy = zlink.RidDuplicatePolicy.HANDOVER
        with pytest.raises(ValueError):
            options.rid_duplicate_policy = invalid
        assert options.rid_duplicate_policy is zlink.RidDuplicatePolicy.HANDOVER
