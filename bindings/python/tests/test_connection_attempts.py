# SPDX-License-Identifier: MPL-2.0

"""Projection of Core's connection attempt snapshot and POLLCONNECT readiness
(bindings spec: connection_attempts_snapshot, PollEventFlag.POLLCONNECT)."""

import socket as _pysocket
import time

import pytest

import zlink

WAIT_MS = 5_000


def _tcp_endpoint():
    with _pysocket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        return f"tcp://127.0.0.1:{reserve.getsockname()[1]}"


def _wait_attempt(socket, state, deadline_ms=WAIT_MS):
    deadline = time.monotonic() + deadline_ms / 1000.0
    while True:
        rows = socket.connection_attempts_snapshot()
        if rows and rows[0].state == state:
            return rows[0]
        assert time.monotonic() < deadline, f"attempt never reached {state!r}: {rows!r}"
        time.sleep(0.01)


def test_values_match_core_abi():
    assert int(zlink.PollEventFlag.POLLCONNECT) == 128
    assert [int(v) for v in zlink.ConnectionAttemptState] == [0, 1, 2]
    assert [int(v) for v in zlink.ConnectionFailureKind] == list(range(10))
    assert [int(v) for v in zlink.ConnectionFailureStage] == list(range(7))


def test_snapshot_reports_connecting_connected_and_failed_rows():
    with zlink.create_context() as ctx:
        client = zlink.create_dealer_socket(ctx)
        server = zlink.create_router_socket(ctx)
        try:
            client.options.linger_ms = 0
            server.options.linger_ms = 0
            assert client.connection_attempts_snapshot() == []

            endpoint = _tcp_endpoint()
            client.connect(endpoint)
            rows = client.connection_attempts_snapshot()
            assert len(rows) == 1
            row = rows[0]
            assert isinstance(row, zlink.ConnectionAttempt)
            assert row.endpoint == endpoint
            assert row.intent_id != 0 and row.revision != 0
            assert row.last_failure_id == 0 or row.state != zlink.ConnectionAttemptState.CONNECTED

            failed = _wait_attempt(client, zlink.ConnectionAttemptState.ATTEMPT_FAILED)
            assert failed.intent_id == row.intent_id
            assert failed.last_failure_id != 0
            assert isinstance(failed.last_failure, zlink.ConnectionFailure)
            assert failed.last_failure.kind == zlink.ConnectionFailureKind.REFUSED
            assert isinstance(failed.last_failure.stage, zlink.ConnectionFailureStage)

            server.bind(endpoint)
            connected = _wait_attempt(client, zlink.ConnectionAttemptState.CONNECTED)
            assert connected.intent_id == row.intent_id
            assert connected.revision != failed.revision
            # A failure survives the later success; the id only changes on a new failure.
            assert connected.last_failure_id == failed.last_failure_id

            client.disconnect(endpoint)
            assert client.connection_attempts_snapshot() == []
        finally:
            client.close()
            server.close()


def test_pollconnect_reports_connection_state_change():
    with zlink.create_context() as ctx:
        client = zlink.create_dealer_socket(ctx)
        server = zlink.create_router_socket(ctx)
        try:
            client.options.linger_ms = 0
            server.options.linger_ms = 0
            endpoint = _tcp_endpoint()
            server.bind(endpoint)
            with zlink.create_poller() as poller:
                events = zlink.create_poll_events(1)
                poller.add_socket(client, zlink.PollEventFlag.POLLCONNECT, 9)
                client.connect(endpoint)
                deadline = time.monotonic() + WAIT_MS / 1000.0
                while True:
                    count = poller.wait(events, 50)
                    if count == 1 and events.revents(0) & zlink.PollEventFlag.POLLCONNECT:
                        break
                    assert time.monotonic() < deadline, "POLLCONNECT never fired"
            _wait_attempt(client, zlink.ConnectionAttemptState.CONNECTED)
        finally:
            client.close()
            server.close()


@pytest.mark.parametrize(
    "factory",
    [
        "create_pair_socket",
        "create_dealer_socket",
        "create_router_socket",
        "create_pub_socket",
        "create_sub_socket",
        "create_xpub_socket",
        "create_xsub_socket",
    ],
)
def test_every_connecting_socket_has_snapshot(factory):
    with zlink.create_context() as ctx:
        sock = getattr(zlink, factory)(ctx)
        try:
            assert sock.connection_attempts_snapshot() == []
        finally:
            sock.close()


def test_stream_socket_has_no_snapshot():
    with zlink.create_context() as ctx:
        sock = zlink.create_stream_socket(ctx)
        try:
            assert not hasattr(sock, "connection_attempts_snapshot")
        finally:
            sock.close()
