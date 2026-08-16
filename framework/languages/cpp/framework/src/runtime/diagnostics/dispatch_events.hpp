/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

namespace zlink::framework
{

enum class message_flow_outcome_t
{
    received = 0,
    dispatched = 1,
    replied = 2,
    dropped = 3,
    sent = 4,
    reply_received = 5,
    admitted = 6,
    completed = 7,
    backpressured = 8
};

enum class dispatch_error_surface_t
{
    channel = 0,
    route_mesh_channel = 1,
    spot_route = 2,
    spot_subscription = 3,
    spot_actor = 4,
    stream_session = 5,
    node = 6,
    instance_spot = 7,
    actor_relocation = 8,
    classic_fanout = 9
};

enum class dispatch_message_kind_t
{
    request = 0,
    send = 1,
    publish = 2,
    response = 3,
    error = 4,
    actor_request = 5,
    actor_send = 6,
    control = 7
};

enum class dispatch_error_reason_t
{
    handler_missing = 0,
    payload_decode_failed = 1,
    handler_exception = 2,
    invalid_frame = 3,
    reply_path_missing = 4,
    unexpected_reply = 5,
    backpressure = 6,
    stale_target = 7,
    shutdown = 8
};

enum class dispatch_error_action_t
{
    reply_error = 0,
    drop = 1,
    fail_caller = 2
};

enum class message_flow_result_t
{
    succeeded = 0,
    failed = 1,
    backpressured = 2,
    dropped = 3,
    cancelled = 4,
    shutdown = 5
};

enum class message_flow_reason_t
{
    backpressure = 0,
    stale_target = 1,
    target_closed = 2,
    shutdown = 3,
    location_unavailable = 4,
    activation_rejected = 5,
    activation_timeout = 6
};

struct message_dispatch_error_event_t
{
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    dispatch_error_reason_t reason;
    dispatch_error_action_t action;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> correlation_id;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::string> mesh_name;
    std::optional<std::string> channel_route_kind;
    std::optional<std::string> target_rid;
    std::optional<std::string> server_rid;
    std::optional<std::string> instance_spot_type;
    std::optional<std::string> activation_state;
};

struct message_flow_event_t
{
    message_flow_outcome_t outcome;
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> correlation_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::size_t> message_size;
    std::optional<dispatch_error_reason_t> error_reason;
    std::optional<dispatch_error_action_t> error_action;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::string> detail_stage;
    std::optional<std::string> detail_result;
    std::optional<std::string> mesh_name;
    std::optional<std::string> channel_route_kind;
    std::optional<std::string> target_rid;
    std::optional<std::string> server_rid;
    std::optional<std::string> instance_spot_type;
    std::optional<std::string> activation_state;
    std::optional<double> duration_seconds;
    std::optional<std::uint64_t> source_mesh_generation;
    std::optional<message_flow_result_t> result;
    std::optional<message_flow_reason_t> reason;
};

} // namespace zlink::framework
