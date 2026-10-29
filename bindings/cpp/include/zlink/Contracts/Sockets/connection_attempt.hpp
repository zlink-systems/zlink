/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <zlink/socket/api.h>

#include <cstdint>
#include <string>

namespace zlink
{

enum class connection_attempt_state_t : uint32_t
{
    connecting = ZLINK_CONNECTION_CONNECTING,
    connected = ZLINK_CONNECTION_CONNECTED,
    attempt_failed = ZLINK_CONNECTION_ATTEMPT_FAILED
};

enum class connection_failure_kind_t : uint32_t
{
    none = ZLINK_CONNECTION_FAILURE_NONE,
    refused = ZLINK_CONNECTION_FAILURE_REFUSED,
    endpoint_missing = ZLINK_CONNECTION_FAILURE_ENDPOINT_MISSING,
    timed_out = ZLINK_CONNECTION_FAILURE_TIMED_OUT,
    unreachable = ZLINK_CONNECTION_FAILURE_UNREACHABLE,
    name_resolution = ZLINK_CONNECTION_FAILURE_NAME_RESOLUTION,
    auth_failed = ZLINK_CONNECTION_FAILURE_AUTH_FAILED,
    protocol_failed = ZLINK_CONNECTION_FAILURE_PROTOCOL_FAILED,
    connection_lost = ZLINK_CONNECTION_FAILURE_CONNECTION_LOST,
    other = ZLINK_CONNECTION_FAILURE_OTHER
};

enum class connection_failure_stage_t : uint32_t
{
    none = ZLINK_CONNECTION_STAGE_NONE,
    resolve = ZLINK_CONNECTION_STAGE_RESOLVE,
    transport = ZLINK_CONNECTION_STAGE_TRANSPORT,
    tls = ZLINK_CONNECTION_STAGE_TLS,
    websocket = ZLINK_CONNECTION_STAGE_WEBSOCKET,
    core_handshake = ZLINK_CONNECTION_STAGE_CORE_HANDSHAKE,
    established = ZLINK_CONNECTION_STAGE_ESTABLISHED
};

struct connection_failure_t
{
    connection_failure_kind_t kind = connection_failure_kind_t::none;
    connection_failure_stage_t stage = connection_failure_stage_t::none;
    int32_t normalized_errno = 0;
    uint32_t disconnect_reason = 0;
};

struct connection_attempt_t
{
    std::string endpoint;
    // Opaque equality tokens: intent_id and revision are nonzero in snapshots.
    uint64_t intent_id = 0;
    uint64_t revision = 0;
    // Zero denotes no failure history; success preserves previous failures.
    uint64_t last_failure_id = 0;
    connection_attempt_state_t state = connection_attempt_state_t::connecting;
    connection_failure_t last_failure;
};

} // namespace zlink
