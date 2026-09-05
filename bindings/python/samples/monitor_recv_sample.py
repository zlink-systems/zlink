import socket as _socket

import zlink


def _reserve_tcp_port():
    sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port

def main():
    port = _reserve_tcp_port()
    endpoint = f"tcp://127.0.0.1:{port}"

    with zlink.create_context() as ctx:
        with zlink.create_pair_socket(ctx) as server:
            with zlink.create_pair_socket(ctx) as client:
                with server.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as server_monitor:
                    with client.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as client_monitor:
                        if server_monitor.status().is_ready():
                            raise AssertionError("monitor sample expected idle server snapshot")
                        if client_monitor.status().is_ready():
                            raise AssertionError("monitor sample expected idle client snapshot")
                        if server_monitor.status().is_ready():
                            raise AssertionError("monitor sample expected idle server snapshot before connect")
                        if client_monitor.status().is_ready():
                            raise AssertionError("monitor sample expected idle client snapshot before connect")
                        server.bind(endpoint)
                        client.connect(endpoint)
                        with zlink.create_poller() as poller:
                            monitors = (server_monitor, client_monitor)
                            for slot, monitor in enumerate(monitors):
                                poller.add_monitor(monitor, zlink.PollEventFlag.POLLIN, slot)
                            events = zlink.create_poll_events(2)
                            pending = {0, 1}
                            while pending:
                                if poller.wait(events, 5000) == 0:
                                    raise TimeoutError("monitor sample expected CONNECTION_READY")
                                for index in range(events.ready_count):
                                    slot = events.slot(index)
                                    monitor = monitors[slot]
                                    while (event := monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)) is not None:
                                        if int(event.event) & int(zlink.MonitorEventMask.CONNECTION_READY):
                                            pending.discard(slot)
                            for monitor in monitors:
                                poller.remove_monitor(monitor)
                print('[monitor/recv] recv: "connection-ready"')


if __name__ == "__main__":
    main()
