# SPDX-License-Identifier: MPL-2.0

import ctypes
import time

from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError, RecvError
from ...contracts.eventing.codes import PollEventFlag, PollSourceKind
from ...contracts.eventing.poller import PollEvent, PollEvents
from ...contracts.sockets.codes import RecvResult
from ..._native.ffi import ZlinkPollerEvent, lib
from ..handles.native_support import _raise_last_error, _raise_result_error


class NativePollEvents:
    def __init__(self, capacity):
        capacity = int(capacity)
        if capacity <= 0:
            raise ValueError("capacity must be > 0")
        self._capacity = capacity
        self._events = (ZlinkPollerEvent * capacity)()
        self._ready_count = 0

    @property
    def capacity(self):
        return self._capacity

    @property
    def ready_count(self):
        return self._ready_count

    def source_kind(self, index):
        event = self._event(index)
        return PollSourceKind(int(event.source_kind))

    def slot(self, index):
        return int(self._event(index).user_data or 0)

    def revents(self, index):
        return int(self._event(index).events)

    def has_event(self, index, event):
        return (self.revents(index) & int(event)) != 0

    def fd(self, index):
        return int(self._event(index).fd)

    def event(self, index):
        native = self._event(index)
        return PollEvent(
            source_kind=PollSourceKind(int(native.source_kind)),
            slot=int(native.user_data or 0),
            revents=int(native.events),
            fd=int(native.fd),
        )

    def _mark_ready_count(self, count):
        count = int(count)
        if count < 0 or count > self._capacity:
            raise ValueError("ready count out of range")
        self._ready_count = count

    def _event(self, index):
        index = int(index)
        if index < 0 or index >= self._ready_count:
            raise IndexError(f"ready index {index}")
        return self._events[index]


class NativePoller:
    def __init__(self):
        if hasattr(self, "_handle"):
            return
        self._handle = lib().zlink_poller_new()
        if not self._handle:
            _raise_last_error()
        self._socket_registrations = {}

    @staticmethod
    def _completion_owner(socket, events):
        if int(events) & int(PollEventFlag.POLLCOMPLETION):
            return getattr(socket, "_completion_owner", None)
        return None

    def add_socket(self, socket, events, slot):
        user_data = ctypes.c_void_p(_validate_slot(slot))
        owner = self._completion_owner(socket, events)
        if owner is not None:
            owner.transfer_to_public(self)
        rc = lib().zlink_poller_add(self._handle, socket._handle, user_data, int(events))
        if rc != 0:
            if owner is not None:
                owner.transfer_to_runtime(self)
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        self._socket_registrations[int(socket._handle)] = [socket, int(events), owner]

    def add_fd(self, fd, events, slot):
        user_data = ctypes.c_void_p(_validate_slot(slot))
        rc = lib().zlink_poller_add_fd(self._handle, fd, user_data, int(events))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def add_timer(self, timer, slot):
        user_data = ctypes.c_void_p(_validate_slot(slot))
        rc = lib().zlink_poller_add_timer(self._handle, timer._handle, user_data)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def modify_socket(self, socket, events):
        key = int(socket._handle)
        registration = self._socket_registrations.get(key)
        old_owner = None if registration is None else registration[2]
        new_owner = self._completion_owner(socket, events)
        if old_owner is None and new_owner is not None:
            new_owner.transfer_to_public(self)
        rc = lib().zlink_poller_modify(self._handle, socket._handle, int(events))
        if rc != 0:
            if old_owner is None and new_owner is not None:
                new_owner.transfer_to_runtime(self)
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        if registration is not None:
            registration[1] = int(events)
            registration[2] = new_owner
        if old_owner is not None and new_owner is None:
            old_owner.transfer_to_runtime(self)

    def modify_fd(self, fd, events):
        rc = lib().zlink_poller_modify_fd(self._handle, int(fd), int(events))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def remove_socket(self, socket):
        key = int(socket._handle)
        rc = lib().zlink_poller_remove(self._handle, socket._handle)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        registration = self._socket_registrations.pop(key, None)
        if registration is not None and registration[2] is not None:
            registration[2].transfer_to_runtime(self)

    def remove_fd(self, fd):
        rc = lib().zlink_poller_remove_fd(self._handle, int(fd))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def remove_timer(self, timer):
        rc = lib().zlink_poller_remove_timer(self._handle, timer._handle)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def size(self):
        error_out = ctypes.c_int()
        rc = lib().zlink_poller_size(self._handle, ctypes.byref(error_out))
        if rc < 0:
            _raise_result_error(ConfigError, ConfigResult, error_out.value, lib().zlink_errno())
        return int(rc)

    def wait(self, events, timeout_ms):
        if not isinstance(events, PollEvents):
            raise TypeError("events must be PollEvents")
        timeout_ms = int(timeout_ms)
        deadline = (
            time.monotonic() + timeout_ms / 1000.0 if timeout_ms > 0 else None
        )
        native_timeout = timeout_ms
        while True:
            error_out = ctypes.c_int()
            ready = lib().zlink_poller_wait(
                self._handle,
                events._events,
                events.capacity,
                native_timeout,
                ctypes.byref(error_out),
            )
            if ready < 0:
                _raise_result_error(
                    RecvError,
                    RecvResult,
                    error_out.value,
                    lib().zlink_errno(),
                )
            output_index = 0
            for index in range(int(ready)):
                native = events._events[index]
                native_flags = int(native.events)
                if int(native.source_kind) == int(PollSourceKind.SOCKET):
                    registration = self._socket_registrations.get(
                        int(native.socket or 0)
                    )
                    owner = None if registration is None else registration[2]
                    completion_ready = bool(
                        native_flags & int(PollEventFlag.POLLCOMPLETION)
                    )
                    writable_retry_ready = bool(
                        native_flags & int(PollEventFlag.POLLOUT)
                    ) and owner is not None and owner.has_managed_writable_wait()
                    if owner is not None and (
                        completion_ready or writable_retry_ready
                    ):
                        drained = owner.drain(self)
                        if completion_ready and drained.request_count == 0:
                            native_flags &= ~int(PollEventFlag.POLLCOMPLETION)
                            native.events = native_flags
                if native_flags == 0:
                    continue
                if output_index != index:
                    events._events[output_index] = native
                output_index += 1
            events._mark_ready_count(output_index)
            if output_index != 0 or timeout_ms == 0 or ready == 0:
                return output_index

            # A WRITABLE-only record is internal SEND or pre-admission REQUEST
            # progress. If the caller watched only POLLCOMPLETION, keep waiting
            # for a REQUEST record within the original deadline instead of
            # returning a false event.
            if deadline is not None:
                remaining_ms = int((deadline - time.monotonic()) * 1000.0)
                if remaining_ms <= 0:
                    return 0
                native_timeout = remaining_ms

    def close(self):
        if not self._handle:
            return
        handle = ctypes.c_void_p(self._handle)
        rc = lib().zlink_poller_destroy(ctypes.byref(handle))
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self._handle = None
        registrations_map = getattr(self, "_socket_registrations", {})
        registrations = list(registrations_map.values())
        registrations_map.clear()
        for _socket, _events, owner in registrations:
            if owner is not None:
                owner.transfer_to_runtime(self)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


def _validate_slot(slot):
    if not isinstance(slot, int) or slot < 0:
        raise ValueError("slot must be a non-negative int")
    if slot > ctypes.c_size_t(-1).value:
        raise ValueError("slot is too large for uintptr_t")
    return slot


NativePoller.__module__ = "zlink.contracts.eventing.poller"
NativePoller.__name__ = "Poller"
NativePoller.__qualname__ = "Poller"
NativePollEvents.__module__ = "zlink.contracts.eventing.poller"
NativePollEvents.__name__ = "PollEvents"
NativePollEvents.__qualname__ = "PollEvents"


def create_poller():
    return NativePoller()


def create_poll_events(capacity):
    return NativePollEvents(capacity)
