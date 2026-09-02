# SPDX-License-Identifier: MPL-2.0

from .native_parts import _materialize_native_parts


def _timeout_to_ms(timeout):
    if timeout in (None, 0):
        return 0
    milliseconds = int(float(timeout) * 1000)
    if milliseconds < 0:
        raise ValueError("timeout must not be negative")
    return min(0xFFFFFFFF, max(1, milliseconds))


_clone_payload = _materialize_native_parts
