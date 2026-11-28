# SPDX-License-Identifier: MPL-2.0

_MAX_TIMEOUT_MS = 0xFFFFFFFF


def _timeout_to_ms(timeout):
    if timeout in (None, 0):
        return 0
    milliseconds = float(timeout) * 1000
    if not 0 <= milliseconds <= _MAX_TIMEOUT_MS:
        raise ValueError(
            f"timeout must be between 0 and {_MAX_TIMEOUT_MS / 1000} seconds"
        )
    return max(1, int(milliseconds))
