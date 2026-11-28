# SPDX-License-Identifier: MPL-2.0

import errno
import socket

import pytest
import zlink


@pytest.mark.parametrize("mode", [zlink.StreamRecvMode.RAW, zlink.StreamRecvMode.PACKET])
def test_bound_stream_rejects_incompatible_receive_family_and_mode_change(mode):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        endpoint = f"tcp://127.0.0.1:{probe.getsockname()[1]}"
    with (
        zlink.create_context() as context,
        zlink.create_stream_socket(context) as stream,
        zlink.StreamPacket() as packet,
        zlink.create_received() as received,
    ):
        stream.stream_options.recv_mode = mode
        stream.bind(endpoint)
        if mode is zlink.StreamRecvMode.RAW:
            compatible = lambda: stream.recv_into(received, flags=zlink.RecvFlags.DONT_WAIT)
            incompatible = lambda: stream.recv_packet_into(packet, flags=zlink.RecvFlags.DONT_WAIT)
        else:
            compatible = lambda: stream.recv_packet_into(packet, flags=zlink.RecvFlags.DONT_WAIT)
            incompatible = lambda: stream.recv_into(received, flags=zlink.RecvFlags.DONT_WAIT)

        assert compatible() is False
        with pytest.raises(zlink.RecvError) as error:
            incompatible()
        assert error.value.result is zlink.RecvResult.NOT_SUPPORTED
        assert error.value.internal_errno == errno.ENOTSUP

        for attempted in (zlink.StreamRecvMode.RAW, zlink.StreamRecvMode.PACKET):
            with pytest.raises(zlink.ConfigError) as error:
                stream.stream_options.recv_mode = attempted
            assert error.value.result is zlink.ConfigResult.INVALID_STATE
            assert error.value.internal_errno == errno.EBUSY
            assert stream.stream_options.recv_mode is mode


@pytest.mark.parametrize("packet_first", [False, True])
def test_stream_packet_mode_and_notify_are_mutually_exclusive(packet_first):
    with zlink.create_context() as context, zlink.create_stream_socket(context) as stream:
        options = stream.stream_options
        if packet_first:
            options.recv_mode = zlink.StreamRecvMode.PACKET
        else:
            options.notify = True
        with pytest.raises(zlink.ConfigError) as error:
            if packet_first:
                options.notify = True
            else:
                options.recv_mode = zlink.StreamRecvMode.PACKET
        assert error.value.result is zlink.ConfigResult.NOT_SUPPORTED
        assert error.value.internal_errno == errno.ENOTSUP
        assert options.notify is (not packet_first)
        assert options.recv_mode is (
            zlink.StreamRecvMode.PACKET if packet_first else zlink.StreamRecvMode.UNSPECIFIED
        )
