# SPDX-License-Identifier: MPL-2.0

import inspect

import pytest
import zlink


@pytest.mark.parametrize(
    ("method", "error"),
    [
        (zlink.StreamSocket.bind, "BindError"),
        (zlink.StreamSocket.unbind, "ConnectError"),
        (zlink.StreamSocket.set_tls_server, "ConfigError"),
        (zlink.StreamSocket.set_tls_client, "ConfigError"),
        (zlink.StreamSocket.recv_packet_into, "RecvError"),
        (zlink.DealerSocket.connection_attempts_snapshot, "ConfigError"),
        (zlink.Received.first_part, "RecvError"),
        (zlink.Received.single_part_or_throw, "RecvError"),
        (zlink.Received.reply, "SubmitError"),
        (zlink.SendOp.submit, "SubmitError"),
        (zlink.SendOp.submit_sync, "SubmitError"),
        (zlink.RequestOp.submit, "SubmitError"),
        (zlink.RequestOp.submit_sync, "RequestError"),
        (zlink.ReplyOp.submit, "SubmitError"),
        (zlink.PublishOp.submit, "SubmitError"),
    ],
)
def test_public_method_documents_raised_error(method, error):
    doc = inspect.getdoc(method) or ""
    _, marker, raises = doc.partition("Raises:\n")

    assert marker
    assert f"{error}:" in raises
