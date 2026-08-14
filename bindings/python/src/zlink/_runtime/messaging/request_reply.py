# SPDX-License-Identifier: MPL-2.0

import errno

from ...contracts.errors.errors import SubmitError
from ...contracts.sockets.codes import SubmitResult
from .native_parts import _materialize_native_parts


_ERRNO_ENOTSUP = getattr(errno, "ENOTSUP", getattr(errno, "EOPNOTSUPP", 95))


def _ensure_reply_flags_supported(flags):
    if int(flags) != 0:
        raise SubmitError(SubmitResult.NOT_SUPPORTED, _ERRNO_ENOTSUP)


def _timeout_to_ms(timeout):
    if timeout in (None, 0):
        return 0
    milliseconds = int(float(timeout) * 1000)
    if milliseconds < 0:
        raise ValueError("timeout must not be negative")
    return min(0xFFFFFFFF, max(1, milliseconds))


_clone_payload = _materialize_native_parts
