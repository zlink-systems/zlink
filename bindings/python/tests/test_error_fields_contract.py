# SPDX-License-Identifier: MPL-2.0

import errno

import pytest

import zlink


@pytest.mark.parametrize(
    ("error_type", "code"),
    [(zlink.ZlinkError, 1), (zlink.ConfigError, zlink.ConfigResult.INVALID_ARGUMENT)],
)
def test_internal_errno_constructor_and_display(error_type, code):
    error = error_type(code, internal_errno=errno.EINVAL)

    assert error.internal_errno == errno.EINVAL
    assert f"internal_errno={errno.EINVAL}" in str(error)
    assert f"internal_errno={errno.EINVAL}" in repr(error)


def test_native_errno_warns_and_delegates():
    error = zlink.ConfigError(zlink.ConfigResult.INVALID_ARGUMENT, errno.EINVAL)

    with pytest.warns(DeprecationWarning, match="internal_errno") as warnings:
        legacy_value = error.native_errno

    assert warnings[0].filename == __file__
    assert legacy_value == error.internal_errno == errno.EINVAL
