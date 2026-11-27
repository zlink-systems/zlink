# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno
from unittest.mock import Mock, patch

import pytest

import zlink
from zlink._runtime.sockets import socket_base


LONG_FILTER = "x" * 4096


@pytest.mark.parametrize("create_socket", [zlink.create_sub_socket, zlink.create_xsub_socket])
@pytest.mark.parametrize(
    ("filter_", "index", "expected", "results"),
    [
        pytest.param("", 1, None, [zlink.ConfigResult.NOT_FOUND], id="missing"),
        pytest.param("", 0, ("", False), [zlink.ConfigResult.OK], id="empty"),
        pytest.param(
            LONG_FILTER, 0, (LONG_FILTER, False),
            [zlink.ConfigResult.BUFFER_TOO_SMALL, zlink.ConfigResult.OK], id="long",
        ),
        pytest.param(
            LONG_FILTER, 0, None,
            [zlink.ConfigResult.BUFFER_TOO_SMALL, zlink.ConfigResult.NOT_FOUND],
            id="removed-before-retry",
        ),
        pytest.param(
            "", 0, zlink.ConfigError, [zlink.ConfigResult.INVALID_ARGUMENT],
            id="invalid-argument",
        ),
    ],
)
def test_subscription_at_core_calls(create_socket, filter_, index, expected, results):
    with zlink.create_context() as ctx, create_socket(ctx) as socket:
        socket.set_subscription(filter_)
        native = socket_base.lib()
        calls = []

        def subscription_at(handle, index, buffer, size, pattern):
            length = ctypes.cast(size, ctypes.POINTER(ctypes.c_size_t))
            capacity = length[0] if buffer is not None else 0
            if results[0] == zlink.ConfigResult.INVALID_ARGUMENT:
                length[0] = len(LONG_FILTER)
                rc = zlink.ConfigResult.INVALID_ARGUMENT
            elif calls and results[-1] == zlink.ConfigResult.NOT_FOUND:
                socket.unset_subscription(filter_)
                rc = native.zlink_subscription_at(handle, index, buffer, size, pattern)
            else:
                rc = native.zlink_subscription_at(handle, index, buffer, size, pattern)
            calls.append((capacity, rc))
            return rc

        fake = Mock(wraps=native)
        fake.zlink_subscription_at.side_effect = subscription_at
        if expected is zlink.ConfigError:
            fake.zlink_errno.return_value = errno.EINVAL
        with patch.object(socket_base, "lib", return_value=fake):
            if expected is zlink.ConfigError:
                with pytest.raises(zlink.ConfigError) as error:
                    socket.subscription_at(index)
                assert error.value.result == zlink.ConfigResult.INVALID_ARGUMENT
            else:
                assert socket.subscription_at(index) == expected

        assert [rc for _, rc in calls] == results
        assert calls[0][0] > 0
        if len(results) == 2:
            assert calls[1][0] == len(LONG_FILTER)
