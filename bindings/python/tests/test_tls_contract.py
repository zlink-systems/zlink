# SPDX-License-Identifier: MPL-2.0

from pathlib import Path
import socket

import pytest
import zlink


@pytest.mark.skipif(not zlink.has("tls"), reason="Core built without TLS")
def test_tls_pair_multipart_round_trip():
    certs = Path(__file__).resolve().parents[3] / "core/tests/certs/gen"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        endpoint = f"tls://127.0.0.1:{probe.getsockname()[1]}"

    with (
        zlink.create_context() as context,
        zlink.create_pair_socket(context) as server,
        zlink.create_pair_socket(context) as client,
        zlink.create_received() as received,
    ):
        for peer in (server, client):
            peer.options.linger_ms = 0
            peer.options.send_timeout_ms = 3000
            peer.options.receive_timeout_ms = 3000

        server.set_tls_server(str(certs / "server.crt"), str(certs / "server.key"))
        client.set_tls_client(str(certs / "ca.crt"), "localhost")
        server.bind(endpoint)
        client.connect(endpoint)

        for sender, receiver, payload in (
            (client, server, (b"request\x00", b"")),
            (server, client, (b"", b"reply\xff")),
        ):
            sender.send().messages(*payload).submit_sync()
            assert receiver.recv_into(received)
            assert received.to_bytes_list() == list(payload)
