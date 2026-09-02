# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import threading
import errno
from typing import Optional

from ..core.routing_id import RoutingId
from ..errors.errors import RecvError
from ..sockets.codes import RecvResult
from .message import Message


class StreamPacket:
    """Reusable caller-owned output for one STREAM packet."""

    routing_id: Optional[RoutingId]
    header: Optional[Message]
    body: Optional[Message]

    def __init__(self) -> None:
        self.routing_id = None
        self.header = None
        self.body = None
        self._receive_lock = threading.Lock()

    @property
    def is_empty(self) -> bool:
        return self.routing_id is None and self.header is None and self.body is None

    def _reset_unlocked(self) -> None:
        header = self.header
        body = self.body
        self.routing_id = None
        self.header = None
        self.body = None
        if header is not None:
            header.close()
        if body is not None:
            body.close()

    def _begin_receive(self) -> None:
        if not self._receive_lock.acquire(blocking=False):
            raise RecvError(RecvResult.BUSY, errno.EBUSY)
        try:
            self._reset_unlocked()
        except BaseException:
            self._receive_lock.release()
            raise

    def _finish_receive(self, routing_id=None, header=None, body=None) -> None:
        self.routing_id = routing_id
        self.header = header
        self.body = body
        self._receive_lock.release()

    def close(self) -> None:
        with self._receive_lock:
            self._reset_unlocked()

    def __enter__(self) -> "StreamPacket":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


__all__ = ["StreamPacket"]
