# SPDX-License-Identifier: MPL-2.0

"""Monitor event and status fields carry the typed enums, not raw ints."""

import socket as _pysocket
import time

import zlink


def _tcp_endpoint():
    with _pysocket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        return f"tcp://127.0.0.1:{reserve.getsockname()[1]}"


def test_enum_values_match_core_abi():
    assert [int(v) for v in zlink.TransportLane] == [0, 1]
    assert zlink.TransportLane.APPLICATION == 0
    assert zlink.TransportLane.COMPLETION == 1
    assert int(zlink.MonitorSourceKind.SOCKET) == 1
    assert int(zlink.MonitorStateFlag.READY) == 1
    assert int(zlink.MonitorStateFlag.BOUND_READY) == 2
    assert int(zlink.MonitorStateFlag.CLOSED) == 8
    assert int(zlink.MonitorStatusDetail.SND_PENDING_MSGS) == 2
    assert int(zlink.MonitorStatusDetail.RCV_PENDING_MSGS) == 4
    assert int(zlink.MonitorStatusDetail.AUTO_HWM_BUDGET) == 8
    assert int(zlink.MonitorStatusDetail.AUTO_HWM_BUFFERS) == 16
    assert int(zlink.MonitorStatusDetail.FLOW_STATE) == 32


def test_monitor_event_and_status_are_typed():
    with zlink.create_context() as ctx:
        server = zlink.create_router_socket(ctx)
        client = zlink.create_dealer_socket(ctx)
        try:
            server.options.linger_ms = 0
            client.options.linger_ms = 0
            with client.monitor_open() as monitor:
                status = monitor.status()
                assert isinstance(status.source_kind, zlink.MonitorSourceKind)
                assert isinstance(status.state_flags, zlink.MonitorStateFlag)
                assert isinstance(status.detail_flags, zlink.MonitorStatusDetail)

                endpoint = _tcp_endpoint()
                server.bind(endpoint)
                client.connect(endpoint)
                deadline = time.monotonic() + 5
                event = None
                while event is None and time.monotonic() < deadline:
                    event = monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)
                    time.sleep(0.001)
                assert event is not None
                assert isinstance(event.event, zlink.MonitorEventMask)
                assert isinstance(event.transport_lane, zlink.TransportLane)
                assert isinstance(event.flags, zlink.MonitorEventFlag)
        finally:
            client.close()
            server.close()
