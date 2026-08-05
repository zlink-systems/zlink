# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from .received import _BaseReceived


@runtime_checkable
class TopicMessage(_BaseReceived, Protocol):
    """A received publish: its topic and message parts. Owns its parts until
    closed."""

    @property
    def topic(self):
        """The topic the message was published under."""
        ...

    @topic.setter
    def topic(self, value): ...


__all__ = ["TopicMessage"]
