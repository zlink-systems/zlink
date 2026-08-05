# SPDX-License-Identifier: MPL-2.0

import ctypes
from typing import Optional

from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.sockets.codes import RecvResult, SubmitResult
from ...contracts.errors.errors import CloseError, ConfigError, RecvError, SubmitError
from ..._native.ffi import ZlinkMsg, lib
from ..._runtime.handles.native_support import (
    _init_msg_from_buffer,
    _msg_data_ptr,
    _msg_refcnt,
    _msg_size,
    _msg_to_bytes,
    _raise_result_error,
)


# Per-message metadata key range.
METADATA_KEY_USER_MIN = 0x0100
METADATA_VALUE_MAX = 65535


class ReceivedMessage:
    def __init__(self, msg=None, routing_id=None, *, owner=None, index=None):
        self._msg = msg
        self._owner = owner
        self._index = index
        self._closed = False
        self.routing_id = routing_id

    @classmethod
    def _from_owner(cls, owner, index, routing_id=None):
        message = cls.__new__(cls)
        message._msg = None
        message._owner = owner
        message._index = index
        message._closed = False
        message.routing_id = routing_id
        return message

    def _native_msg(self):
        if self._owner is not None:
            return self._owner.msg(self._index)
        if self._closed:
            raise RuntimeError("received message is closed")
        return self._msg

    def __len__(self):
        if self._owner is not None:
            return self._owner.size(self._index)
        return _msg_size(self._native_msg())

    @property
    def data(self):
        """Memoryview over a snapshot of this received part.

        Received parts may be closed independently from the returned view. The
        view therefore points at Python-owned bytes instead of native receive
        storage.
        """
        if self._owner is not None:
            return self._owner.data(self._index)
        native = self._native_msg()
        return memoryview(_msg_to_bytes(native))

    def to_bytes(self):
        if self._owner is not None:
            return self._owner.to_bytes(self._index)
        return _msg_to_bytes(self._native_msg())

    def close(self):
        if self._closed:
            return
        if self._owner is not None:
            self._owner.close_part(self._index)
            self._closed = True
            return
        rc = lib().zlink_msg_close(ctypes.byref(self._msg))
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


class _BaseReceived:
    """Shared parts/lifecycle for received-message containers.

    ReceivedMultipart and TopicMessage previously carried byte-identical
    copies of the iterator, length, byte-extraction, single-part, and
    context-manager methods; they only diverge in which routing fields
    their __init__/_adopt_from manage. The common surface lives here so
    the two stay in lockstep.
    """

    _owner = None
    parts = ()

    @staticmethod
    def _build_parts(owner):
        if owner._part_count == 1:
            return (ReceivedMessage._from_owner(owner, 0),)
        return tuple(
            ReceivedMessage._from_owner(owner, index)
            for index in range(owner._part_count)
        )

    def _close_current_owner(self):
        if self._owner is None:
            return
        self._owner.close()

    def _attach_owner(self, owner):
        self._owner = owner
        self.parts = self._build_parts(owner)

    def _clear_owner(self):
        self._owner = None
        self.parts = ()

    def __iter__(self):
        return iter(self.parts)

    def __len__(self):
        return len(self.parts)

    def to_bytes_list(self):
        if self._owner is not None:
            if hasattr(self._owner, "to_bytes"):
                return [
                    self._owner.to_bytes(index)
                    for index in range(self._owner._part_count)
                ]
            return [
                _msg_to_bytes(self._owner.msg(index))
                for index in range(self._owner._part_count)
            ]
        return [message.to_bytes() for message in self.parts]

    def is_single_part(self):
        return len(self.parts) == 1

    def first_part(self):
        if not self.parts:
            raise RecvError(RecvResult.NO_DATA, 0)
        return self.parts[0]

    def single_part_or_throw(self):
        if len(self.parts) != 1:
            raise RecvError(RecvResult.NO_DATA, 0)
        return self.parts[0]

    def _last_part_data(self):
        owner = self._owner
        if owner is not None and owner._part_count > 0:
            return owner.data(owner._part_count - 1)
        if not self.parts:
            return memoryview(b"")
        return self.parts[-1].data

    def close(self):
        if self._owner is None:
            return
        self._owner.close()
        self._clear_owner()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


class ReceivedMultipart(_BaseReceived):
    def __init__(
        self,
        owner=None,
        routing_id=None,
        request_seq=None,
        *,
        router_socket=None,
    ):
        # HOT PATH: ReceivedMultipart() and Received() create empty storage
        # objects that recv_into refills in place through the public
        # caller-provided receive contract.
        if owner is None:
            self._owner = None
            self.parts = ()
            self.routing_id = None
            self.request_seq = None
            self._router_socket = None
            return
        self._owner = owner
        if owner._part_count == 1:
            self.parts = (ReceivedMessage._from_owner(owner, 0),)
        else:
            self.parts = self._build_parts(owner)
        self.routing_id = routing_id
        self.request_seq = request_seq
        self._router_socket = router_socket

    def _adopt_from(self, source):
        """Replace this Received's internal state with the contents of
        ``source``. Closes any state currently held first; ``source`` is
        left detached after the call."""
        if source is self:
            return
        self._close_current_owner()
        self._owner = source._owner
        self.parts = source.parts
        self.routing_id = source.routing_id
        self.request_seq = source.request_seq
        self._router_socket = source._router_socket
        source._clear_owner()
        source.routing_id = None
        source.request_seq = None
        source._router_socket = None

    def _replace(
        self,
        owner,
        routing_id=None,
        request_seq=None,
        *,
        router_socket=None,
    ):
        current_owner = self._owner
        if current_owner is not None:
            current_owner.close()
        self._owner = owner
        if owner._part_count == 1:
            self.parts = (ReceivedMessage._from_owner(owner, 0),)
        else:
            self.parts = self._build_parts(owner)
        self.routing_id = routing_id
        self.request_seq = request_seq
        self._router_socket = router_socket


class TopicMessage(_BaseReceived):
    def __init__(
        self,
        topic=None,
        owner=None,
        routing_id=None,
        request_seq=None,
    ):
        if owner is None:
            self._topic = ""
            self._topic_raw = None
            self._owner = None
            self.parts = ()
            self.routing_id = None
            self.request_seq = None
            return
        self._topic = topic
        self._topic_raw = None
        self._owner = owner
        self.parts = self._build_parts(owner)
        self.routing_id = routing_id
        self.request_seq = request_seq

    @property
    def topic(self):
        raw = self._topic_raw
        if raw is not None:
            self._topic = raw.decode("utf-8", errors="replace")
            self._topic_raw = None
        return self._topic

    @topic.setter
    def topic(self, value):
        self._topic = value
        self._topic_raw = None

    def _adopt_from(self, source):
        if source is self:
            return
        self._close_current_owner()
        self._topic = source._topic
        self._topic_raw = source._topic_raw
        self._owner = source._owner
        self.parts = source.parts
        self.routing_id = source.routing_id
        self.request_seq = source.request_seq
        source._topic = ""
        source._topic_raw = None
        source._clear_owner()
        source.routing_id = None
        source.request_seq = None

    def _replace(
        self,
        owner,
        *,
        topic="",
        topic_raw=None,
        routing_id=None,
        request_seq=None,
    ):
        self._close_current_owner()
        self._topic = topic
        self._topic_raw = topic_raw
        self._attach_owner(owner)
        self.routing_id = routing_id
        self.request_seq = request_seq


class Received(ReceivedMultipart):
    def send(self):
        """Return a SendOp routed back to the source of this Received.

        Source routing is encapsulated; accumulate payload via
        ``.message(...)`` before calling ``.submit()``.
        """
        if self._router_socket is None or self.routing_id is None:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        return self._router_socket.send(self.routing_id)

    def reply(self):
        """Return a ReplyOp for this received request. Valid only when
        ``request_seq`` is present."""
        if self.request_seq is None:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        if self._router_socket is None or self.routing_id is None:
            raise SubmitError(SubmitResult.INVALID_STATE, 0)
        return self._router_socket.reply(self.routing_id, self.request_seq)


class SubscriptionEvent:
    def __init__(self, topic="", subscribed=False, routing_id=None):
        self.routing_id = routing_id
        self.topic = topic
        self.subscribed = subscribed

    def _adopt_from(self, source):
        if source is self:
            return
        self.routing_id = source.routing_id
        self.topic = source.topic
        self.subscribed = source.subscribed
        source.routing_id = None
        source.topic = ""
        source.subscribed = False


class Message:
    def __init__(self, size: Optional[int] = None):
        self._msg = ZlinkMsg()
        self._valid = False
        self._keepalive = None
        if size is None:
            rc = lib().zlink_msg_init(ctypes.byref(self._msg))
        else:
            if size < 0:
                raise ValueError("size must be >= 0")
            rc = lib().zlink_msg_init_size(ctypes.byref(self._msg), size)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        self._valid = True

    @classmethod
    def allocate(cls, size: int):
        return cls(size)

    @classmethod
    def from_(cls, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        elif isinstance(data, Message):
            data = data.data
        msg = cls.__new__(cls)
        msg._msg = ZlinkMsg()
        msg._valid = False
        msg._keepalive = _init_msg_from_buffer(msg._msg, data, borrow=False)
        msg._valid = True
        return msg

    def copy(self):
        return type(self).from_(self)

    def size(self):
        return _msg_size(self._msg) if self._valid else 0

    def is_empty(self):
        return self.size() == 0

    @property
    def data(self):
        if not self._valid:
            return memoryview(b"")
        # Cache the memoryview keyed by (ptr, size). The underlying msg can
        # only be mutated by close()/_adopt_from()-style transitions, both of
        # which clear `_valid` and therefore invalidate this cache via the
        # ``not self._valid`` short-circuit above. Reading `.data` repeatedly
        # — common when forwarding a payload between parts of a pipeline —
        # would otherwise allocate a fresh `from_address` view every call.
        ptr = _msg_data_ptr(self._msg)
        size = self.size()
        if not ptr or size <= 0:
            return memoryview(b"")
        cache = getattr(self, "_data_view_cache", None)
        if cache is not None and cache[0] == ptr and cache[1] == size:
            return cache[2]
        view = memoryview((ctypes.c_ubyte * size).from_address(ptr)).cast("B")
        self._data_view_cache = (ptr, size, view)
        return view

    def to_bytes(self):
        return _msg_to_bytes(self._msg) if self._valid else b""

    def copy_to(self, destination, source_offset=0, destination_offset=0, length=None):
        payload_size = self.size()
        if length is None:
            length = payload_size - source_offset
        if (
            source_offset < 0
            or destination_offset < 0
            or length < 0
            or source_offset + length > payload_size
        ):
            raise ValueError("copy range is out of bounds")
        view = memoryview(destination)
        if view.readonly:
            raise TypeError("destination must be writable")
        if view.ndim != 1 or view.format != "B":
            try:
                view = view.cast("B")
            except TypeError as exc:
                raise TypeError("destination must be a writable byte buffer") from exc
        if destination_offset + length > view.nbytes:
            raise ValueError("destination buffer is too small")
        if length:
            source = self.data
            source_ptr = ctypes.addressof(
                (ctypes.c_ubyte * source.nbytes).from_buffer(source)
            )
            destination_ptr = ctypes.addressof(
                (ctypes.c_ubyte * view.nbytes).from_buffer(view)
            )
            ctypes.memmove(
                destination_ptr + destination_offset,
                source_ptr + source_offset,
                length,
            )
        return length

    def try_copy_to(self, destination):
        # Keep the capacity miss distinct from invalid destination input. The
        # public contract returns the copied byte count, or None only when the
        # destination cannot hold this payload.
        if memoryview(destination).nbytes < self.size():
            return None
        return self.copy_to(destination)

    def to_string(self, encoding="utf-8"):
        return self.to_bytes().decode(encoding)

    def ref_count(self):
        return _msg_refcnt(self._msg) if self._valid else -1

    def close(self):
        if not self._valid:
            return
        rc = lib().zlink_msg_close(ctypes.byref(self._msg))
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self._valid = False
        self._keepalive = None
        self._data_view_cache = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


for _public_type in (
    ReceivedMessage,
    ReceivedMultipart,
    Received,
    TopicMessage,
    SubscriptionEvent,
    Message,
):
    _public_type.__module__ = "zlink.contracts.messaging.message"


def message_from(data):
    return Message.from_(data)


def message_allocate(size):
    return Message.allocate(size)


def wrap_message_buffer(data):
    msg = Message.__new__(Message)
    msg._msg = ZlinkMsg()
    msg._valid = False
    msg._keepalive = _init_msg_from_buffer(msg._msg, data, borrow=True)
    msg._valid = True
    return msg


def create_received_message(*args, **kwargs):
    return ReceivedMessage(*args, **kwargs)


def received_message_from_owner(owner, index, routing_id=None):
    return ReceivedMessage._from_owner(owner, index, routing_id)


def create_received_multipart(*args, **kwargs):
    return ReceivedMultipart(*args, **kwargs)


def create_received(*args, **kwargs):
    return Received(*args, **kwargs)


def create_topic_message(*args, **kwargs):
    return TopicMessage(*args, **kwargs)


def create_subscription_event(*args, **kwargs):
    return SubscriptionEvent(*args, **kwargs)
