/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>
#include <zlink/stream_connector/contracts/compression.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <memory>
#include <string>

namespace zlink::stream_connector
{

struct heartbeat_options_t
{
    /// Enables connector heartbeat control frames.
    bool enabled = true;

    /// Minimum time between heartbeat pings while the connection is idle.
    std::chrono::milliseconds interval{1000};

    /// Maximum idle time before the connector treats the connection as disconnected.
    std::chrono::milliseconds timeout{5000};
};

struct reconnect_options_t
{
    /// Enables reconnect attempts after connection loss or initial connect failure.
    bool enabled = true;

    /// Delay before the first reconnect attempt.
    std::chrono::milliseconds initial_delay{250};

    /// Maximum delay between reconnect attempts.
    std::chrono::milliseconds max_delay{5000};

    /// Multiplier applied to the reconnect delay after each failed attempt.
    double backoff_factor = 2.0;

    /// Maximum reconnect attempts. nullopt means no attempt limit.
    std::optional<int> max_attempts = 3;
};

struct connector_options_t
{
    /// Remote stream endpoint URI.
    std::string endpoint;

    /// Transport used to open the endpoint.
    transport_t transport = transport_t::tcp;

    /// Maximum time allowed for a connect attempt, including configured reconnect attempts.
    std::chrono::milliseconds connect_timeout{5000};

    /// Default timeout for request/reply operations.
    std::chrono::milliseconds request_timeout{30000};

    /// Default timeout for wait_for operations.
    std::chrono::milliseconds wait_timeout{5000};

    /// Heartbeat behavior for this connector.
    heartbeat_options_t heartbeat;

    /// Reconnect behavior for this connector.
    reconnect_options_t reconnect;

    /// Maximum encoded payload bytes accepted by send and request calls.
    std::size_t max_send_payload_size = 64 * 1024;

    /// Maximum encoded payload bytes accepted from inbound stream frames.
    std::size_t max_receive_payload_size = 64 * 1024;

    /// Maximum pending inbound observer notifications before new notifications are dropped.
    std::size_t max_inbound_observer_notifications = 1024;

    /// Maximum received push packets kept for manual dispatch or wait_for().
    ///
    /// When the queue is full, newer unmatched push packets are dropped and reported through
    /// on_error(...). Request/reply frames are still routed to their pending request.
    std::size_t max_received_messages = 1024;

    /// Maximum payload bytes copied into an inbound observer snapshot.
    std::size_t max_inbound_observer_payload_preview_bytes = 0;

    /// Disables TLS server certificate validation when true.
    bool skip_server_certificate_validation = false;

    /// Controls when On(...) callbacks run for received push packets.
    ///
    /// In manual mode, callbacks run only when dispatch() is called. In immediate mode, callbacks
    /// run from the connector receive path. wait_for(...) consumes matching packets directly in
    /// either mode.
    dispatch_mode_t dispatch_mode = dispatch_mode_t::manual;

    /// Default compression preference for connector calls that opt into compression.
    compression_t compression = compression_t::lz4;

    /// Codec used when payload_compressed is set. nullptr disables compression.
    std::shared_ptr<const compression_codec_t> compression_codec = lz4_compression_codec ();
};

} // namespace zlink::stream_connector
