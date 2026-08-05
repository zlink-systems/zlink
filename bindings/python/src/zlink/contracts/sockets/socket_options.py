# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


def _stub_get(self):
    ...


def _stub_set(self, value):
    ...


def _contract_property():
    return property(_stub_get, _stub_set)


@runtime_checkable
class CommonSocketOptions(Protocol):
    """Typed facade over the socket options shared by every socket type.

    Attributes:
        linger_ms: How long, in ms, ``close`` waits to deliver queued messages.
        send_high_water_mark: Max outbound messages queued before back-pressure; 0 means no limit.
        receive_high_water_mark: Max inbound messages queued before back-pressure; 0 means no limit.
        send_timeout_ms: How long, in ms, a blocking send waits before failing.
        receive_timeout_ms: How long, in ms, a blocking receive waits before failing.
        immediate: Whether messages queue only to fully established connections.
        rid_duplicate_policy: How the socket reacts to a peer reusing a routing id.
        connect_timeout_ms: Time limit, in ms, for a single connection attempt.
        ipv6: Whether IPv6 connections are enabled.
        tcp_no_delay: Whether Nagle's algorithm is disabled (``TCP_NODELAY``).
        tcp_keepalive: Whether TCP keep-alive probes are enabled.
        heartbeat_interval_ms: Interval, in ms, between heartbeat pings on an idle connection.
        heartbeat_ttl_ms: How long, in ms, the remote keeps the connection alive without a heartbeat.
        heartbeat_timeout_ms: How long, in ms, to wait for a heartbeat reply before treating the connection as dead.
        max_message_size: Maximum inbound message size in bytes; -1 means no limit.
        backlog: Maximum length of the pending-connection queue on a bound endpoint.
        reconnect_interval_ms: Base delay, in ms, before reconnecting a broken connection.
        reconnect_interval_max_ms: Maximum reconnect delay, in ms, under exponential backoff.
        submit_retry_mode: Whether a submit that fails locally is retried.
        submit_retry_timeout_ms: Total time budget, in ms, for submit retries.
        submit_retry_attempts: Maximum number of submit retry attempts.
    """

    linger_ms = _contract_property()
    send_high_water_mark = _contract_property()
    receive_high_water_mark = _contract_property()
    send_timeout_ms = _contract_property()
    receive_timeout_ms = _contract_property()
    immediate = _contract_property()
    rid_duplicate_policy = _contract_property()
    connect_timeout_ms = _contract_property()
    ipv6 = _contract_property()
    tcp_no_delay = _contract_property()
    tcp_keepalive = _contract_property()
    heartbeat_interval_ms = _contract_property()
    heartbeat_ttl_ms = _contract_property()
    heartbeat_timeout_ms = _contract_property()
    max_message_size = _contract_property()
    backlog = _contract_property()
    reconnect_interval_ms = _contract_property()
    reconnect_interval_max_ms = _contract_property()
    submit_retry_mode = _contract_property()
    submit_retry_timeout_ms = _contract_property()
    submit_retry_attempts = _contract_property()


@runtime_checkable
class DealerSocketOptions(Protocol):
    """Typed facade over DEALER-specific socket options.

    Attributes:
        probe: Whether to send an empty probe on connect so the peer learns this socket's routing id.
        weight: This peer's relative load-balancing weight (0-100).
        request_timeout_ms: How long, in ms, a DEALER request waits for a reply.
    """

    probe = _contract_property()
    weight = _contract_property()
    request_timeout_ms = _contract_property()


@runtime_checkable
class StreamSocketOptions(Protocol):
    """Typed facade over STREAM-specific socket options.

    Attributes:
        notify: Whether peer connect and disconnect events are delivered as messages.
    """

    notify = _contract_property()


@runtime_checkable
class SubSocketOptions(Protocol):
    """Typed facade over SUB-specific socket options."""

    @property
    def topics_count(self):
        """The number of active subscriptions on this socket."""
        ...


@runtime_checkable
class PubSocketOptions(Protocol):
    """Typed facade over PUB/XPUB-specific socket options.

    Attributes:
        verbose: Whether every subscription message (including duplicates) is delivered.
        verboser: Whether every subscription and unsubscription message is delivered.
        manual: Whether subscriptions are handled manually via ``approve_subscribe``/``reject_subscribe``.
        no_drop: Whether a back-pressured send raises an error instead of dropping the message.
        manual_last_value: Manual handling that replays the last cached message per topic to a new subscriber.
        welcome_message: A message automatically sent to each newly connected subscriber.
    """

    verbose = _contract_property()
    verboser = _contract_property()
    manual = _contract_property()
    no_drop = _contract_property()
    manual_last_value = _contract_property()
    welcome_message = _contract_property()

    @property
    def topics_count(self):
        """The number of distinct topics currently subscribed across
        subscribers."""
        ...

    def approve_subscribe(self, routing_id):
        """Accept a pending subscription from ``routing_id``; requires manual
        mode."""
        ...

    def reject_subscribe(self, routing_id):
        """Decline a pending subscription from ``routing_id``; requires manual
        mode."""
        ...


@runtime_checkable
class RouterSocketOptions(Protocol):
    """Typed facade over ROUTER-specific socket options.

    Attributes:
        mandatory: Whether sending to an unknown route raises an error instead of dropping the message.
        handover: Whether a new peer reusing an existing routing id takes it over.
        probe: Whether to send an empty probe on connect so the peer learns this socket's routing id.
        connect_routing_id: The routing id assigned to the next outgoing connection.
        weight: This peer's relative load-balancing weight (0-100).
        request_timeout_ms: How long, in ms, a ROUTER request waits for a reply.
    """

    mandatory = _contract_property()
    handover = _contract_property()
    probe = _contract_property()
    connect_routing_id = _contract_property()
    weight = _contract_property()
    request_timeout_ms = _contract_property()
