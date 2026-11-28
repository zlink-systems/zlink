# SPDX-License-Identifier: MPL-2.0

import pytest
import zlink


def test_endpoint_accepts_exactly_255_utf8_bytes():
    endpoint = "inproc://" + "é" * 123
    assert len(endpoint.encode()) == 255
    with (
        zlink.create_context() as context,
        zlink.create_pair_socket(context) as server,
        zlink.create_pair_socket(context) as client,
        zlink.create_received() as received,
    ):
        server.options.receive_timeout_ms = 1000
        server.bind(endpoint)
        client.connect(endpoint)
        client.send().message(b"exact endpoint").submit_sync()
        assert server.recv_into(received)
        assert received.to_bytes_list() == [b"exact endpoint"]
        client.disconnect(endpoint)
        server.unbind(endpoint)
        for operation in (server.bind, server.unbind, client.connect, client.disconnect):
            with pytest.raises(ValueError, match="255"):
                operation(endpoint + "a")
