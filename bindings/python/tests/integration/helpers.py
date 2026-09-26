import socket
import time

import zlink

ZLINK_PAIR = 0
ZLINK_PUB = 1
ZLINK_SUB = 2
ZLINK_DEALER = 5
ZLINK_ROUTER = 6
ZLINK_XPUB = 9
ZLINK_XSUB = 10

ZLINK_SNDMORE = 2


def get_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def transports(prefix):
    return [
        ("tcp", ""),
        ("ws", ""),
        ("inproc", f"inproc://{prefix}-{int(time.time() * 1000)}"),
    ]


def endpoint_for(name, base_endpoint, suffix):
    if name == "inproc":
        return base_endpoint + suffix
    port = get_port()
    return f"{name}://127.0.0.1:{port}"


def try_transport(name, fn):
    fn()


def wait_connected(*monitors, timeout_s=5.0):
    del timeout_s
    for monitor in monitors:
        event = monitor.recv()
        if int(event.event) != int(zlink.MonitorEventMask.CONNECTION_READY):
            raise AssertionError(f"unexpected monitor event: {event.event!r}")
