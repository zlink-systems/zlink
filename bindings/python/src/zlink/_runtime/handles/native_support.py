# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno as _errno
import sys
import traceback

from ...contracts.core.routing_id import RoutingId
from ...contracts.errors.codes import (
    ConfigResult,
)
from ...contracts.sockets.codes import (
    RecvResult,
    RequestResult,
)
from ...contracts.errors.errors import (
    ConfigError,
    RecvError,
    ZlinkError,
    _TypedZlinkError,
)
from ..._native.ffi import ZlinkMsg, ZlinkRoutingId, lib


def _request_result_from_code(code):
    try:
        return RequestResult(int(code))
    except ValueError:
        # Keep a future Core result observable to the caller. Mapping it to a
        # known terminal result would change the wire contract silently.
        return int(code)


def _config_result_from_errno(err):
    if err == 0:
        return ConfigResult.OK
    if err == _errno.EFAULT:
        return ConfigResult.INVALID_HANDLE
    if err == _errno.EINVAL:
        return ConfigResult.INVALID_ARGUMENT
    if err in (_errno.ENOTSUP, getattr(_errno, "EOPNOTSUPP", _errno.ENOTSUP)):
        return ConfigResult.NOT_SUPPORTED
    return ConfigResult.INVALID_ARGUMENT


def _raise_zlink_error(error_type, result, native_errno=None):
    if native_errno is None:
        native_errno = lib().zlink_errno()
    raise error_type(result, native_errno)


def _raise_result_error(error_type, result_type, rc, native_errno=None):
    try:
        result = result_type(int(rc))
    except ValueError:
        # Do not map an unknown native result to enum value zero. The typed
        # error preserves the raw result for forward-compatible diagnostics.
        result = int(rc)
    _raise_zlink_error(error_type, result, native_errno)


def _raise_last_error():
    err = lib().zlink_errno()
    raise ZlinkError(err, err)


def _raise_mapped_error(error_type, mapper, native_errno=None):
    if native_errno is None:
        native_errno = lib().zlink_errno()
    _raise_zlink_error(error_type, mapper(int(native_errno)), int(native_errno))


def _raise_config_error_from_errno(native_errno=None):
    _raise_mapped_error(ConfigError, _config_result_from_errno, native_errno)


def _as_bytes_view(data):
    if isinstance(data, bytes):
        return memoryview(data)
    if hasattr(data, "to_bytes") and callable(data.to_bytes):
        return memoryview(data.to_bytes())
    try:
        view = memoryview(data)
    except TypeError as exc:
        raise TypeError("data must support the buffer protocol") from exc
    if view.ndim != 1 or view.format != "B":
        try:
            view = view.cast("B")
        except TypeError:
            view = memoryview(bytes(view))
    if not view.c_contiguous:
        view = memoryview(bytes(view))
    return view


def _send_buffer(data):
    if isinstance(data, bytes):
        size = len(data)
        if size == 0:
            return None, 0, data
        return ctypes.c_char_p(data), size, data

    view = _as_bytes_view(data)
    size = view.nbytes
    if size == 0:
        return None, 0, view
    if view.readonly:
        raw = view.tobytes()
        return ctypes.c_char_p(raw), size, raw
    return ctypes.addressof((ctypes.c_char * size).from_buffer(view)), size, view


def _validated_int32(value, *, field="value"):
    native = int(value)
    if native < -(1 << 31) or native > ((1 << 31) - 1):
        raise OverflowError(f"{field} must fit in signed 32-bit range")
    return native


def _validated_uint32(value, *, field="value"):
    native = int(value)
    if native < 0 or native > ((1 << 32) - 1):
        raise OverflowError(f"{field} must fit in unsigned 32-bit range")
    return native


def _validated_int64(value, *, field="value"):
    native = int(value)
    if native < -(1 << 63) or native > ((1 << 63) - 1):
        raise OverflowError(f"{field} must fit in signed 64-bit range")
    return native


def _validated_c_string_bytes(data, *, field="value", max_length=None):
    raw = bytes(_as_bytes_view(data))
    if b"\0" in raw:
        raise ValueError(f"{field} must not contain NUL bytes")
    if max_length is not None and len(raw) > max_length:
        raise ValueError(f"{field} must be at most {max_length} bytes")
    return raw


def _validated_c_string_value(value, *, field="value", max_length=None):
    if isinstance(value, str):
        return _validated_c_string_text(value, field=field, max_length=max_length)
    return _validated_c_string_bytes(value, field=field, max_length=max_length)


def _validated_c_string_text(text, *, field="value", max_length=None):
    if "\0" in text:
        raise ValueError(f"{field} must not contain NUL characters")
    raw = text.encode()
    if max_length is not None and len(raw) > max_length:
        raise ValueError(f"{field} must be at most {max_length} bytes")
    return raw


def _decode_topic_text(raw):
    return bytes(raw).decode("utf-8", errors="replace")


_SOCKET_RECV_HANDLER = ctypes.CFUNCTYPE(
    None,
    ctypes.POINTER(ZlinkRoutingId),
    ctypes.POINTER(ZlinkMsg),
    ctypes.c_size_t,
    ctypes.c_void_p,
)
_SOCKET_SEND_READY_HANDLER = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_void_p,
)
_REPLY_HANDLER = ctypes.CFUNCTYPE(
    None,
    ctypes.c_int,
    ctypes.POINTER(ZlinkMsg),
    ctypes.c_size_t,
    ctypes.c_void_p,
)
def _copy_routing_id(routing_id):
    view = _as_bytes_view(routing_id)
    size = view.nbytes
    if size <= 0 or size > 255:
        raise ValueError("routing_id length must be between 1 and 255")
    native = ZlinkRoutingId()
    native.size = size
    for index in range(size):
        native.data[index] = view[index]
    return native


def _validated_routing_id_bytes(routing_id):
    if isinstance(routing_id, bytes):
        size = len(routing_id)
        if size <= 0 or size > 255:
            raise ValueError("routing_id length must be between 1 and 255")
        return routing_id
    native = _copy_routing_id(routing_id)
    return bytes(native.data[: native.size])


def _msg_data_ptr(msg):
    return lib().zlink_msg_data(ctypes.byref(msg))


def _msg_size(msg):
    return int(lib().zlink_msg_size(ctypes.byref(msg)))


def _msg_refcnt(msg):
    error_out = ctypes.c_int()
    value = int(lib().zlink_msg_refcnt(ctypes.byref(msg), ctypes.byref(error_out)))
    if value < 0:
        _raise_result_error(
            ConfigError,
            ConfigResult,
            error_out.value,
            lib().zlink_errno(),
        )
    return value


def _msg_to_bytes(msg):
    size = _msg_size(msg)
    if size <= 0:
        return b""
    ptr = _msg_data_ptr(msg)
    if not ptr:
        return b""
    return ctypes.string_at(ptr, size)


def _init_msg_from_buffer(msg, data, *, borrow):
    ptr, size, keepalive = _send_buffer(data)
    if borrow:
        data_ptr = ctypes.c_void_p(ptr if isinstance(ptr, int) else ctypes.cast(ptr, ctypes.c_void_p).value or 0)
        rc = lib().zlink_msg_init_data(ctypes.byref(msg), data_ptr, size, None, None)
    else:
        rc = lib().zlink_msg_init_size(ctypes.byref(msg), size)
        if rc == 0 and size:
            ctypes.memmove(_msg_data_ptr(msg), ptr, size)
    if rc != 0:
        _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
    # A copied message no longer depends on the caller buffer. Borrowed
    # messages retain the source object until the native message is closed.
    return keepalive if borrow else None


def _clone_native_msg(src):
    dst = ZlinkMsg()
    rc = lib().zlink_msg_init(ctypes.byref(dst))
    if rc != 0:
        _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
    rc = lib().zlink_msg_copy(ctypes.byref(dst), ctypes.byref(src))
    if rc != 0:
        lib().zlink_msg_close(ctypes.byref(dst))
        _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
    return dst


def _close_multipart(parts_ptr, part_count):
    if parts_ptr and part_count:
        lib().zlink_multipart_close(parts_ptr, part_count)


def _routing_id_bytes(routing_id):
    raw = bytes(routing_id.data[: routing_id.size])
    if not raw:
        return None
    return RoutingId(raw)


def _is_eagain(exc):
    return isinstance(exc, ZlinkError) and exc.native_errno == _errno.EAGAIN


def _report_unhandled_callback_exception(handler):
    exc_type, exc_value, exc_traceback = sys.exc_info()
    if exc_type is None:
        return
    print(f"Unhandled zlink callback exception in {handler!r}",
          file=sys.stderr)
    traceback.print_exception(exc_type, exc_value, exc_traceback)


class _ReceivedPartsOwner:
    def __init__(self, parts_ptr, part_count):
        self._parts_ptr = parts_ptr
        self._part_count = part_count
        self._closed = False
        self._open_parts = [True] * part_count

    def msg(self, index):
        if self._closed or not self._open_parts[index]:
            raise RuntimeError("received message is closed")
        return self._parts_ptr[index]

    def size(self, index):
        return _msg_size(self.msg(index))

    def data(self, index):
        return memoryview(_msg_to_bytes(self.msg(index)))

    def to_bytes(self, index):
        return _msg_to_bytes(self.msg(index))

    def close_part(self, index):
        if self._closed or not self._open_parts[index]:
            return
        self._open_parts[index] = False
        if not any(self._open_parts):
            self.close()

    def close(self):
        if self._closed:
            return
        _close_multipart(self._parts_ptr, self._part_count)
        self._parts_ptr = None
        self._open_parts = [False] * self._part_count
        self._closed = True


class _BytesReceivedPartsOwner:
    def __init__(self, parts):
        self._parts = tuple(bytes(part) for part in parts)
        self._part_count = len(self._parts)
        self._closed = False
        self._open_parts = [True] * self._part_count

    @classmethod
    def _from_trusted_bytes_tuple(cls, parts):
        owner = cls.__new__(cls)
        owner._parts = parts
        owner._part_count = len(parts)
        owner._closed = False
        owner._open_parts = [True] * owner._part_count
        return owner

    def _check_open(self, index):
        if self._closed or not self._open_parts[index]:
            raise RuntimeError("received message is closed")

    def msg(self, index):
        self._check_open(index)
        raise RuntimeError("received message does not own a native zlink_msg_t")

    def size(self, index):
        self._check_open(index)
        return len(self._parts[index])

    def data(self, index):
        self._check_open(index)
        return memoryview(self._parts[index])

    def to_bytes(self, index):
        self._check_open(index)
        return self._parts[index]

    def close_part(self, index):
        if self._closed or not self._open_parts[index]:
            return
        self._open_parts[index] = False
        if not any(self._open_parts):
            self.close()

    def close(self):
        if self._closed:
            return
        self._open_parts = [False] * self._part_count
        self._closed = True


def _recv_native_parts(handle, flags):
    # Fast path for single-part messages (the common case): allocate the
    # owner's `ZlinkMsg * 1` array directly and read into its first slot.
    # When `has_more` reports no additional parts we return that array
    # unchanged, skipping the list-then-copy pattern needed for multi-part.
    routing_id = ctypes.POINTER(ZlinkRoutingId)()
    parts_array = (ZlinkMsg * 1)()
    has_more = ctypes.c_int()
    rc = lib().zlink_recv_part(
        handle,
        ctypes.byref(routing_id),
        ctypes.byref(parts_array[0]),
        ctypes.byref(has_more),
        int(flags),
    )
    if rc != 0:
        _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())

    if has_more.value == 0:
        routing = _routing_id_bytes(routing_id.contents) if routing_id else None
        return routing, _ReceivedPartsOwner(parts_array, 1)

    native_parts = [parts_array[0]]
    try:
        while True:
            native_part = ZlinkMsg()
            rc = lib().zlink_recv_part(
                handle,
                ctypes.byref(routing_id),
                ctypes.byref(native_part),
                ctypes.byref(has_more),
                1,
            )
            if rc != 0:
                _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
            native_parts.append(native_part)
            if has_more.value == 0:
                break
    except Exception:
        for native_part in native_parts:
            lib().zlink_msg_close(ctypes.byref(native_part))
        raise

    part_count = len(native_parts)
    final_array = (ZlinkMsg * part_count)()
    for index, native_part in enumerate(native_parts):
        final_array[index] = native_part
    routing = _routing_id_bytes(routing_id.contents) if routing_id else None
    return routing, _ReceivedPartsOwner(final_array, part_count)
