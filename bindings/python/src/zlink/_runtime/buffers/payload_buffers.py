# SPDX-License-Identifier: MPL-2.0
"""Native byte-level conversion helpers.

These helpers translate Python ints/bools to/from the C int32 / int64 byte
layouts the FFI option setters expect. They previously lived in
``contracts/sockets/options.py`` — which forced the runtime layer to import
upward through the contracts package — and have been hoisted here so the
layering stays one-directional (contracts → _runtime, never the reverse).
"""

import ctypes


def _int32_bytes(value):
    native = ctypes.c_int32(_validated_int32(value))
    return ctypes.string_at(ctypes.byref(native), ctypes.sizeof(native))


def _int64_bytes(value):
    native = ctypes.c_int64(_validated_int64(value))
    return ctypes.string_at(ctypes.byref(native), ctypes.sizeof(native))


def _bool_bytes(value):
    return _int32_bytes(1 if value else 0)


def _read_int32(raw):
    if len(raw) != ctypes.sizeof(ctypes.c_int32):
        raise ValueError("native option payload size mismatch")
    return ctypes.c_int32.from_buffer_copy(raw).value


def _read_int64(raw):
    if len(raw) != ctypes.sizeof(ctypes.c_int64):
        raise ValueError("native option payload size mismatch")
    return ctypes.c_int64.from_buffer_copy(raw).value


def _validated_int32(value, *, field="value"):
    if not isinstance(value, int):
        raise TypeError(f"{field} must be int")
    if value < -(2**31) or value > 2**31 - 1:
        raise OverflowError(f"{field} must fit in signed 32-bit range")
    return int(value)


def _validated_int64(value, *, field="value"):
    if not isinstance(value, int):
        raise TypeError(f"{field} must be int")
    if value < -(2**63) or value > 2**63 - 1:
        raise OverflowError(f"{field} must fit in signed 64-bit range")
    return int(value)
