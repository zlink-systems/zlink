# SPDX-License-Identifier: MPL-2.0

import uuid


class RoutingId:
    """An opaque identifier for a messaging peer or route, 1 to 255 bytes long."""

    def __init__(self, data):
        """Create a routing id from a copy of ``data`` (a bytes-like object of 1
        to 255 bytes). Raise :class:`ValueError` when the length is out of
        range."""
        self._raw = _validated_routing_id_bytes(data)

    @classmethod
    def from_(cls, value):
        """Create a routing id from a ``str`` (UTF-8 bytes), an ``int`` (4-byte
        big-endian uint32), a :class:`uuid.UUID` (16 bytes), or a bytes-like
        object."""
        if isinstance(value, str):
            return cls(value.encode("utf-8"))
        if isinstance(value, int):
            if value < 0 or value > 0xFFFFFFFF:
                raise ValueError("routing id uint32 value must be in range 0..4294967295")
            return cls(value.to_bytes(4, "big"))
        if isinstance(value, uuid.UUID):
            return cls(value.bytes)
        return cls(value)

    @classmethod
    def _from_trusted_bytes(cls, value):
        rid = cls.__new__(cls)
        rid._raw = value
        return rid

    @classmethod
    def from_hex(cls, value):
        """Create a routing id by decoding ``value`` as a hex string (non-empty,
        even length, up to 510 digits for 255 bytes). Raise :class:`TypeError`
        or :class:`ValueError` on invalid input."""
        if not isinstance(value, str):
            raise TypeError("value must be str")
        if len(value) == 0 or len(value) % 2 != 0 or any(
            c not in "0123456789abcdefABCDEF" for c in value
        ):
            raise ValueError("routing id string must be a non-empty even-length hex string")
        if len(value) > 510:
            raise ValueError("routing id string must decode to at most 255 bytes")
        return cls(bytes.fromhex(value))

    def to_bytes(self):
        """Return the routing id bytes."""
        return self._raw

    @property
    def size(self):
        """The length of the routing id in bytes."""
        return len(self._raw)

    def __bytes__(self):
        return self._raw

    def __len__(self):
        return len(self._raw)

    def __hash__(self):
        return hash(self._raw)

    def __eq__(self, other):
        if isinstance(other, RoutingId):
            return self._raw == other._raw
        try:
            return self._raw == _validated_routing_id_bytes(other)
        except Exception:
            return False

    def __repr__(self):
        return f"RoutingId({self._raw!r})"

    def to_hex(self):
        """Return the routing id as a lowercase hex string."""
        return self._raw.hex()

    def __str__(self):
        """Return a human-readable form: printable UTF-8 text when possible,
        otherwise a uint, a UUID, or a ``hex:`` fallback."""
        try:
            text = self._raw.decode("utf-8")
            if all(ch.isprintable() or ch == " " for ch in text):
                return text
        except UnicodeDecodeError:
            pass
        if len(self._raw) == 4:
            return str(int.from_bytes(self._raw, "big"))
        if len(self._raw) == 16:
            return str(uuid.UUID(bytes=self._raw))
        return "hex:" + self.to_hex()


def _validated_routing_id_bytes(data):
    raw = bytes(data)
    if len(raw) == 0 or len(raw) > 255:
        raise ValueError("routing_id length must be between 1 and 255")
    return raw
