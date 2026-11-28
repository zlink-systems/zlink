# SPDX-License-Identifier: MPL-2.0

from concurrent.futures import ThreadPoolExecutor
import errno
import threading

import pytest
import zlink
from zlink._native.ffi import lib


def test_concurrent_recv_into_same_packet_is_invalid_state(monkeypatch):
    entered_native = threading.Event()
    release_native = threading.Event()

    def receive_without_data(*args):
        entered_native.set()
        assert release_native.wait(timeout=5)
        return zlink.RecvResult.NO_DATA

    with (
        zlink.create_context() as context,
        zlink.create_stream_socket(context) as first,
        zlink.create_stream_socket(context) as second,
        zlink.StreamPacket() as packet,
        ThreadPoolExecutor(max_workers=1) as executor,
    ):
        monkeypatch.setattr(lib(), "zlink_stream_recv_packet", receive_without_data)
        active = executor.submit(first.recv_packet_into, packet)
        try:
            assert entered_native.wait(timeout=5)
            with pytest.raises(zlink.RecvError) as raised:
                second.recv_packet_into(packet, flags=zlink.RecvFlags.DONT_WAIT)
            assert raised.value.result is zlink.RecvResult.INVALID_STATE
            assert raised.value.internal_errno == errno.EBUSY
        finally:
            release_native.set()
        assert active.result(timeout=5) is False
        assert packet.is_empty
        assert second.recv_packet_into(packet, flags=zlink.RecvFlags.DONT_WAIT) is False
