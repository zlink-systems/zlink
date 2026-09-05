# SPDX-License-Identifier: MPL-2.0

import socket
import uuid

import pytest
import zlink


@pytest.mark.parametrize("transport", ["inproc", "tcp"])
@pytest.mark.parametrize("monitor_alias", [False, True])
def test_monitor_poller_lifecycle(transport, monitor_alias):
    if transport == "tcp":
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            endpoint = f"tcp://127.0.0.1:{reserve.getsockname()[1]}"
    else:
        endpoint = f"inproc://monitor-poller-{uuid.uuid4()}"

    with zlink.create_context() as ctx, zlink.create_router_socket(ctx) as server, \
            zlink.create_dealer_socket(ctx) as client, client.monitor_open(
                zlink.MonitorEventMask.CONNECTION_READY | zlink.MonitorEventMask.DISCONNECTED
            ) as monitor, zlink.create_poller() as poller:
        add = poller.add_monitor if monitor_alias else poller.add_socket
        modify = poller.modify_monitor if monitor_alias else poller.modify_socket
        remove = poller.remove_monitor if monitor_alias else poller.remove_socket
        events = zlink.create_poll_events(1)
        server.bind(endpoint)
        client.connect(endpoint)
        add(monitor, zlink.PollEventFlag.POLLIN, 42)
        assert poller.size() == 1
        modify(monitor, zlink.PollEventFlag.POLLIN)

        def drain(expected):
            assert poller.wait(events, 2000) == 1
            assert events.slot(0) == 42
            assert events.source_kind(0) == zlink.PollSourceKind.SOCKET
            assert events.revents(0) == zlink.PollEventFlag.POLLIN
            received = []
            while (event := monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)) is not None:
                received.append(event.event)
            assert expected in received
            assert poller.wait(events, 0) == 0

        drain(zlink.MonitorEventMask.CONNECTION_READY)
        server.close()
        drain(zlink.MonitorEventMask.DISCONNECTED)
        remove(monitor)
        assert poller.size() == 0
        # Queue a fresh READY event after removal, then use re-registration to
        # prove the source is ready while the old registration stays absent.
        with zlink.create_router_socket(ctx) as replacement:
            replacement.bind(endpoint)
            add(monitor, zlink.PollEventFlag.POLLIN, 42)
            assert poller.wait(events, 2000) == 1
            remove(monitor)
            assert poller.wait(events, 0) == 0
            assert monitor.recv(flags=zlink.RecvFlags.DONT_WAIT).event == zlink.MonitorEventMask.CONNECTION_READY


@pytest.mark.parametrize("monitor_alias", [False, True])
@pytest.mark.parametrize("flags", [zlink.PollEventFlag.POLLOUT,
                                  zlink.PollEventFlag.POLLCOMPLETION,
                                  zlink.PollEventFlag.POLLIN | zlink.PollEventFlag.POLLOUT])
def test_monitor_poller_rejects_invalid_flags(monitor_alias, flags):
    with zlink.create_context() as ctx, zlink.create_dealer_socket(ctx) as socket, \
            socket.monitor_open() as monitor, zlink.create_poller() as poller:
        add = poller.add_monitor if monitor_alias else poller.add_socket
        modify = poller.modify_monitor if monitor_alias else poller.modify_socket
        with pytest.raises(zlink.ConfigError) as error:
            add(monitor, flags, 1)
        assert error.value.result == zlink.ConfigResult.INVALID_ARGUMENT
        assert poller.size() == 0
        add(monitor, zlink.PollEventFlag.POLLIN, 1)
        with pytest.raises(zlink.ConfigError) as error:
            modify(monitor, flags)
        assert error.value.result == zlink.ConfigResult.INVALID_ARGUMENT
        assert poller.size() == 1
        poller.remove_monitor(monitor)
