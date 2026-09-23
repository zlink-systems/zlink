/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>

namespace zlink::stream_connector
{

enum class transport_t
{
    tcp,
    tls,
    websocket,
    websocket_secure
};

enum class codec_t : std::uint8_t
{
    raw = 0,
    json = 1,
    message_pack = 2,
    protobuf = 3
};

enum class compression_t
{
    none,
    lz4
};

enum class dispatch_mode_t
{
    manual,
    immediate
};

enum class message_kind_t : std::uint8_t
{
    send = 1,
    request = 2,
    response = 3,
    error = 4,
    control = 5
};

enum class header_flags_t : std::uint8_t
{
    none = 0,
    has_request_seq = 0x01,
    has_metadata = 0x02,
    payload_compressed = 0x04,
    has_correlation_id = 0x08,
    has_actor_slot = 0x20
};

constexpr header_flags_t operator| (header_flags_t lhs, header_flags_t rhs) noexcept
{
    return static_cast<header_flags_t> (static_cast<std::uint8_t> (lhs)
                                        | static_cast<std::uint8_t> (rhs));
}

/* Closed set of 13 error codes (stream-connector §9.1). An implementation
 * neither adds nor removes a value: the per-error impact table (current
 * operation, connection, close reason, automatic reconnect) is written
 * against exactly these names. */
enum class error_code_t
{
    disconnected,
    configuration_error,
    validation_failed,
    request_timeout,
    connect_timeout,
    frame_decode_failed,
    frame_too_large,
    send_failed,
    compression_failed,
    tls_validation_failed,
    decompression_failed,
    user_callback_failed,
    remote_error
};

enum class connection_state_t
{
    created,
    connecting,
    connected,
    reconnecting,
    disconnected,
    closed
};

/* Closed set of session close reasons (graceful-drain-handoff §7.1; wire
 * values match the session-closing control payload and the runtime-metrics
 * close_reason label). transport failures without a control packet are
 * synthesized as transport_error; a client-initiated close is client_close. */
enum class close_reason_t : std::uint8_t
{
    client_close = 1,
    idle_timeout = 2,
    heartbeat_timeout = 3,
    server_drain = 4,
    protocol_error = 5,
    transport_error = 6
};

} // namespace zlink::stream_connector
