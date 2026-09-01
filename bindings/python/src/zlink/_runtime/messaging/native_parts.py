# SPDX-License-Identifier: MPL-2.0
"""Materialize public payload parts into Core-owned message values.

All send, request, and reply paths use this module as the ownership boundary.
The caller keeps ownership of input buffers and ``Message`` objects because
the returned values are independent staging parts. A synchronous Core call
consumes every staging part it actually attempts on success or ordinary
failure; the binding closes any later part that was not attempted.
"""

import ctypes

from .message_materializer import Message, ReceivedMessage
from ..._native.ffi import ZlinkMsg
from ..handles.native_support import (
    _close_multipart,
    _clone_native_msg,
    _init_msg_from_buffer,
)


def _payload_parts(payload):
    if isinstance(payload, (list, tuple)):
        parts = payload
    else:
        parts = (payload,)
    if not parts:
        raise ValueError("payload must not be empty")
    return parts


def _materialize_native_parts(payload):
    """Create one independently owned native part for each public payload."""

    native_parts = []
    try:
        for part in _payload_parts(payload):
            if isinstance(part, Message):
                native_parts.append(_clone_native_msg(part._msg))
                continue
            if isinstance(part, ReceivedMessage):
                native = part._clone_native_for_send()
                if native is not None:
                    native_parts.append(native)
                    continue

            native = ZlinkMsg()
            _init_msg_from_buffer(native, part, borrow=False)
            native_parts.append(native)
    except Exception:
        for native in native_parts:
            _close_multipart(ctypes.byref(native), 1)
        raise
    return native_parts
