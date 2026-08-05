"""Wire-level stop token shared by python perf single/multi benchmarks.

PERF_SINGLE_TEST_POLICY § 1.4 and PERF_MULTI_TEST_POLICY § 1.3.1 require
that sender / phase termination is signalled on the data wire by sending
this literal token once, instead of using out-of-band ``threading.Event``
flags combined with short polling cadences. Receivers wait with
``poller.wait(events, -1)`` (signal-driven, indefinite) and exit when an inbound
message matches ``STOP_TOKEN``.
"""

from __future__ import annotations


STOP_TOKEN: bytes = b"__zlink_perf_stop__"


def is_stop_token(data) -> bool:
    """Return ``True`` if the raw bytes represent the wire stop token."""

    if data is None:
        return False
    if isinstance(data, (bytes, bytearray, memoryview)):
        return bytes(data) == STOP_TOKEN
    return False


def is_stop_token_in_parts(parts) -> bool:
    """Return ``True`` if any frame in ``parts`` matches the stop token.

    Most patterns send the token as a single-frame message, so checking
    the first frame is enough; we still scan all frames defensively for
    multi-frame transports such as router/dealer-router.
    """

    if not parts:
        return False
    for part in parts:
        if is_stop_token(part):
            return True
    return False
