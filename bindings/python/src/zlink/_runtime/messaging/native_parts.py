# SPDX-License-Identifier: MPL-2.0
"""Project Core whole-message submission onto public payload ownership.

Core consumes the submitted native parts even on rejection. The submitted
parts hold independent references. The binding consumes the caller's
``Message`` on admission or when it retains staging for a WRITABLE wait.
"""

from .message_materializer import Message, ReceivedMessage
from ..._native.ffi import ZlinkMsg
from ...contracts.sockets.codes import SubmitResult
from ..handles.native_support import (
    _close_multipart,
    _clone_native_msg,
    _init_msg_from_buffer,
    _native_extension,
    _as_bytes_view,
)


def _payload_parts(payload):
    if isinstance(payload, (list, tuple)):
        parts = payload
    else:
        parts = (payload,)
    if not parts:
        raise ValueError("payload must not be empty")
    return parts


def _consume_submitted_messages(payload, result, *, retry_retained=False):
    if result == SubmitResult.OK or retry_retained:
        for part in _payload_parts(payload):
            if isinstance(part, Message):
                part.close()


def _materialize_native_parts(payload):
    """Create one independently owned native part for each public payload."""

    if _native_extension is not None:
        return _native_extension.materialize_parts(
            payload, ZlinkMsg, Message, ReceivedMessage, _as_bytes_view
        )

    payload_parts = _payload_parts(payload)
    native_parts = (ZlinkMsg * len(payload_parts))()
    initialized = 0
    try:
        for index, part in enumerate(payload_parts):
            if isinstance(part, Message):
                native_parts[index] = _clone_native_msg(part._msg)
                initialized += 1
                continue
            if isinstance(part, ReceivedMessage):
                native = part._clone_native_for_send()
                if native is not None:
                    native_parts[index] = native
                    initialized += 1
                    continue

            _init_msg_from_buffer(native_parts[index], part)
            initialized += 1
    except Exception:
        _close_multipart(native_parts, initialized)
        raise
    return native_parts
