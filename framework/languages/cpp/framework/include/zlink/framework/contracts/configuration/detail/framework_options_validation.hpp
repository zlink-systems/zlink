/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>

#include <cmath>
#include <set>
#include <string>

namespace zlink::framework::detail
{

inline void validate_dispatch_options (const dispatch_options_t &options)
{
    if (std::isnan (options.diagnostics.sample_rate ()) || options.diagnostics.sample_rate () < 0.0
        || options.diagnostics.sample_rate () > 1.0) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "dispatch diagnostics sample rate must be between 0.0 and 1.0");
    }
}

inline void validate_location_options (const location_options_t &options)
{
    using namespace std::chrono_literals;
    if (options.owner_lease_renew_interval <= 0ms
        || options.owner_lease_ttl <= 0ms
        || options.polling_interval <= 0ms
        || options.store_failure_grace <= 0ms
        || options.owner_lease_fencing_margin <= 0ms
        || options.owner_lease_renew_timeout <= 0ms) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "location lease, polling, failure-grace, fencing, and renewal durations must be greater than zero");
    }
    if (options.owner_lease_renew_interval + options.owner_lease_renew_timeout
        >= options.owner_lease_ttl - options.owner_lease_fencing_margin) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "owner lease renewal interval plus timeout must be shorter than the fenced lease lifetime");
    }
    if (options.route_cache_max_age < 0ms
        || options.message_follow_duration < 0ms) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "route cache age and Message Follow duration must not be negative");
    }
    if (options.route_cache_max_age > 0ms
        && options.message_follow_duration > 0ms
        && options.message_follow_duration
             < options.route_cache_max_age + 5s) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "Message Follow duration must be at least five seconds longer than route cache max age");
    }
    if (options.max_active_outbound_relocations == 0
        || options.max_active_inbound_relocations == 0
        || options.max_concurrent_relocation_captures == 0
        || options.max_concurrent_relocation_restores == 0
        || options.max_relocation_payload_in_flight_bytes == 0) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "relocation concurrency and payload limits must be greater than zero");
    }
}

inline void validate_framework_options (const framework_options_state_t &options,
                                        const handler_group_options_state_t &handler_groups)
{
    validate_dispatch_options (options.dispatch);
    validate_location_options (options.locations);
    if (options.process_memory_limit_bytes
        && *options.process_memory_limit_bytes == 0) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "process memory limit must be positive when specified");
    }
    switch (options.application_hwm_profile) {
        case application_hwm_profile_t::compact:
        case application_hwm_profile_t::low_latency:
        case application_hwm_profile_t::balanced:
        case application_hwm_profile_t::throughput:
            break;
        default:
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "application HWM profile is invalid");
    }
    for (const auto &channel_name : options.client_server_channels) {
        if (!options.client_server_channels_with_server.contains (channel_name)
            && !options.client_server_channels_with_client.contains (channel_name)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "client/server channel '" + channel_name
                                           + "' must enable server or client capability");
        }
        const auto client_count =
          options.client_server_client_registration_counts.contains (channel_name)
            ? options.client_server_client_registration_counts.at (channel_name)
            : 0;
        const auto server_count =
          options.client_server_server_registration_counts.contains (channel_name)
            ? options.client_server_server_registration_counts.at (channel_name)
            : 0;
        if (client_count > 1) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "client/server channel '" + channel_name
                + "' registers the Client role more than once");
        }
        if (server_count > 1) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "client/server channel '" + channel_name
                + "' registers the Server role more than once");
        }
        if (options.route_mesh_channels.contains (channel_name)
            || options.mesh_node_channel_names.contains (channel_name)) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "ChannelName '" + channel_name
                + "' cannot be registered by both ClientServer and RouteMesh");
        }
    }
    for (const auto &channel_name : options.fanout_channels) {
        if (!options.fanout_channels_with_publisher.contains (channel_name)
            && !options.fanout_channels_with_subscriber.contains (channel_name)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "fanout channel '" + channel_name
                                           + "' must enable publisher or subscriber capability");
        }
        if (options.fanout_channels_with_automatic_subscriber.contains (channel_name)
            && options.fanout_channels_with_manual_subscriber.contains (channel_name)) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "fanout channel '" + channel_name
                + "' cannot combine automatic discovery with manual subscriber endpoints");
        }
    }
    for (const auto &channel_name : options.route_mesh_channels) {
        if (!options.route_mesh_channels_with_bind.contains (channel_name)
            && !options.route_mesh_channels_with_client.contains (channel_name)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "route mesh channel '" + channel_name
                                           + "' must enable server or client capability");
        }
    }
    for (const auto &stream_node_name : options.stream_nodes) {
        if (!options.stream_nodes_with_bind.contains (stream_node_name)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM node '" + stream_node_name
                                           + "' must configure a bind endpoint");
        }
        if (!options.stream_nodes_with_session.contains (stream_node_name)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM node '" + stream_node_name
                                           + "' must register a packet session");
        }
    }
    for (const auto &channel_name : options.client_server_channels_with_server) {
        if (!handler_groups.channel_exposes_any (
              channel_name, {handler_group_kind_t::request, handler_group_kind_t::send})) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "client/server channel '" + channel_name
                                           + "' server must map a request or send handler group");
        }
    }
    for (const auto &channel_name : options.fanout_channels_with_subscriber) {
        if (!handler_groups.channel_exposes_any (channel_name, {handler_group_kind_t::publish})) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "fanout channel '" + channel_name
                                           + "' subscriber must map a publish handler group");
        }
    }
}

} // namespace zlink::framework::detail
