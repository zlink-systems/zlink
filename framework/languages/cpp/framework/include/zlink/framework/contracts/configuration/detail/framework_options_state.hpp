/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/configuration/endpoint_connections.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/http/http.hpp>
#include <zlink/framework/contracts/locations/options.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <tuple>
#include <utility>
#include <vector>

namespace zlink::framework
{

enum class application_hwm_profile_t
{
    compact = 0,
    low_latency = 1,
    balanced = 2,
    throughput = 3
};

namespace detail
{

inline bool is_blank (const std::string &value)
{
    return std::all_of (value.begin (), value.end (),
                        [] (unsigned char ch) { return std::isspace (ch) != 0; });
}

inline void require_non_blank (const std::string &value, const char *message)
{
    if (value.empty () || is_blank (value)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error, message);
    }
}

enum class handler_group_kind_t
{
    request = 0,
    send = 1,
    publish = 2
};

template <typename T> concept static_topic_name = requires
{
    {
        T::topic_name
    } -> std::convertible_to<const char *>;
};

template <typename T> concept static_session_name = requires
{
    {
        T::session_name
    } -> std::convertible_to<const char *>;
};

template <typename THandler, typename TPayload> std::string handler_topic_name ()
{
    if constexpr (static_topic_name<THandler>) {
        return THandler::topic_name;
    } else {
        return message_name<TPayload> ();
    }
}

template <typename TSession> std::string stream_session_name ()
{
    if constexpr (static_session_name<TSession>) {
        return TSession::session_name;
    } else {
        return message_name<TSession> ();
    }
}

using manual_connection_map_t =
  std::map<std::string, std::map<std::string, std::vector<std::string>>>;
using stream_session_factory_t = std::function<packet_stream_session_t &(service_provider_t &)>;

inline std::vector<std::string>
manual_connections_for (const manual_connection_map_t &connections_by_node,
                        const std::string &node_name,
                        const std::string &channel_name)
{
    const auto node_connections = connections_by_node.find (node_name);
    if (node_connections == connections_by_node.end ()) {
        return {};
    }
    const auto channel_connections = node_connections->second.find (channel_name);
    if (channel_connections == node_connections->second.end ()) {
        return {};
    }
    return channel_connections->second;
}

inline bool has_manual_connections (const manual_connection_map_t &connections_by_node,
                                    const std::string &node_name,
                                    const std::string &channel_name)
{
    return !manual_connections_for (connections_by_node, node_name, channel_name).empty ();
}

template <typename TSession, typename TDependencies> struct injected_stream_session_registrar_t;

template <typename TDependency>
void ensure_stream_dependency_registered (service_collection_t &services)
{
    if (services.contains (std::type_index (typeid (TDependency)))) {
        return;
    }
    if constexpr (static_dependency_types<TDependency>
                  || std::is_default_constructible_v<TDependency>) {
        injected_stream_session_registrar_t<
          TDependency, typename handler_dependencies_t<TDependency>::type>::add (services);
    }
}

template <typename TSession, typename... TDependencies>
struct injected_stream_session_registrar_t<TSession, dependency_list_t<TDependencies...>>
{
    static void add (service_collection_t &services)
    {
        if (services.contains (std::type_index (typeid (TSession)))) {
            return;
        }
        (ensure_stream_dependency_registered<TDependencies> (services), ...);
        if constexpr (sizeof...(TDependencies) == 0) {
            services.add_scoped<TSession> ();
        } else {
            services.add_scoped<TSession, TDependencies...> ();
        }
    }
};

struct handler_group_options_state_t
{
    using installer_t = std::function<void (const std::string &)>;
    using route_installer_t = std::function<void (route_channel_builder_t &)>;
    using serializer_installer_t = std::function<void ()>;

    struct channel_binding_t
    {
        std::string channel_name;
        std::set<handler_group_kind_t> allowed_kinds;
        std::string surface_name;
    };

    struct installer_binding_t
    {
        handler_group_kind_t kind;
        installer_t installer;
    };

    struct route_installer_binding_t
    {
        handler_group_kind_t kind;
        route_installer_t installer;
    };

    std::map<std::string, std::vector<channel_binding_t>> channels_by_group;
    std::map<std::string, std::vector<installer_binding_t>> installers_by_group;
    std::map<std::string, std::vector<route_installer_binding_t>> route_installers_by_group;
    std::map<std::string, std::set<std::tuple<handler_group_kind_t, std::string, std::string>>>
      handler_packets_by_group;
    void add_channel (const std::string &group_name,
                      const std::string &channel_name,
                      std::set<handler_group_kind_t> allowed_kinds,
                      std::string surface_name)
    {
        require_non_blank (group_name, "handler group name is required");
        auto binding =
          channel_binding_t{channel_name, std::move (allowed_kinds), std::move (surface_name)};
        auto found = installers_by_group.find (group_name);
        if (found != installers_by_group.end ()) {
            for (const auto &installer : found->second) {
                validate_compatible (group_name, binding, installer.kind);
            }
        }

        auto &channels = channels_by_group[group_name];
        channels.push_back (binding);
        if (found == installers_by_group.end ()) {
            return;
        }
        for (const auto &installer : found->second) {
            installer.installer (channel_name);
        }
    }

    void add_installer (std::string group_name, handler_group_kind_t kind, installer_t installer)
    {
        require_non_blank (group_name, "handler group name is required");
        auto found = channels_by_group.find (group_name);
        if (found != channels_by_group.end ()) {
            for (const auto &channel : found->second) {
                validate_compatible (group_name, channel, kind);
            }
        }

        auto &installers = installers_by_group[group_name];
        installers.push_back (installer_binding_t{kind, installer});
        if (found == channels_by_group.end ()) {
            return;
        }
        for (const auto &channel : found->second) {
            installer (channel.channel_name);
        }
    }

    void add_route_installer (std::string group_name,
                              handler_group_kind_t kind,
                              route_installer_t installer)
    {
        require_non_blank (group_name, "handler group name is required");
        auto found = channels_by_group.find (group_name);
        if (found != channels_by_group.end ()) {
            for (const auto &channel : found->second) {
                validate_compatible (group_name, channel, kind);
            }
        }

        auto &installers = route_installers_by_group[group_name];
        installers.push_back (route_installer_binding_t{kind, std::move (installer)});
    }

    void install_route_handlers (zlink_builder_t &zlink) const
    {
        for (const auto &[group_name, channels] : channels_by_group) {
            const auto installers = route_installers_by_group.find (group_name);
            if (installers == route_installers_by_group.end ()) {
                continue;
            }
            for (const auto &channel : channels) {
                if (channel.surface_name != "route mesh channel") {
                    continue;
                }
                auto route_channel = zlink.route_channel (channel.channel_name);
                for (const auto &installer : installers->second) {
                    validate_compatible (group_name, channel, installer.kind);
                    installer.installer (route_channel);
                }
            }
        }
    }

    void add_handler_packet (const std::string &group_name,
                             handler_group_kind_t kind,
                             std::string topic,
                             std::string packet_name)
    {
        require_non_blank (group_name, "handler group name is required");
        auto &packets = handler_packets_by_group[group_name];
        if (!packets.emplace (kind, std::move (topic), std::move (packet_name)).second) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "duplicate handler registration");
        }
    }

    bool channel_exposes_any (const std::string &channel_name,
                              const std::set<handler_group_kind_t> &kinds) const
    {
        for (const auto &[group_name, channels] : channels_by_group) {
            const auto channel_found =
              std::any_of (channels.begin (), channels.end (),
                           [&] (const auto &c) { return c.channel_name == channel_name; });
            if (!channel_found) {
                continue;
            }
            const auto installers = installers_by_group.find (group_name);
            if (installers == installers_by_group.end ()) {
                continue;
            }
            for (const auto &installer : installers->second) {
                if (kinds.contains (installer.kind)) {
                    return true;
                }
            }
        }
        return false;
    }

    static void validate_compatible (const std::string &group_name,
                                     const channel_binding_t &channel,
                                     handler_group_kind_t kind)
    {
        if (channel.allowed_kinds.contains (kind)) {
            return;
        }
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     channel.surface_name + " '" + channel.channel_name
                                       + "' maps handler group '" + group_name
                                       + "' with an incompatible handler kind");
    }
};

struct framework_options_state_t
{
    std::vector<std::function<void (zlink_builder_t &)>> deferred_zlink_actions;
    std::map<std::string, std::function<void (zlink_builder_t &)>> keyed_zlink_actions;
    std::set<std::string> client_server_channels;
    std::set<std::string> fanout_channels;
    std::map<std::string, endpoint_connections_t> client_endpoint_connections;
    std::map<std::string, endpoint_connections_t> subscriber_endpoint_connections;
    std::set<std::string> client_server_channels_with_client;
    std::set<std::string> client_server_channels_with_server;
    std::map<std::string, std::size_t> client_server_client_registration_counts;
    std::map<std::string, std::size_t> client_server_server_registration_counts;
    std::map<std::string, std::string>
      client_server_server_advertise_hosts;
    std::map<std::string, std::optional<std::chrono::milliseconds>>
      client_server_default_request_timeouts;
    std::map<std::string, std::function<void (channel_builder_t &)>>
      client_server_client_actions;
    std::map<std::string, std::function<void (channel_builder_t &)>>
      client_server_server_actions;
    std::set<std::string> fanout_channels_with_publisher;
    std::set<std::string> fanout_channels_with_subscriber;
    std::set<std::string> fanout_channels_with_automatic_subscriber;
    std::set<std::string> fanout_channels_with_manual_subscriber;
    std::set<std::string> route_mesh_channels;
    std::set<std::string> mesh_node_channel_names;
    std::set<std::string> route_mesh_channels_with_bind;
    std::set<std::string> route_mesh_channels_with_client;
    std::set<std::string> stream_nodes;
    std::set<std::string> stream_nodes_with_bind;
    std::set<std::string> stream_nodes_with_session;
    std::set<std::string> stream_nodes_with_actor_dispatch;
    std::set<std::string> stream_session_names;
    std::map<std::string, stream_session_factory_t> stream_session_factories;
    bool has_location_store_instance = false;
    bool has_relocation_store_instance = false;
    location_options_t locations;
    http_options_builder_t http;
    message_metadata_policy_t metadata_policy;
    dispatch_options_t dispatch;
    worker_options_t worker;
    std::optional<std::uint64_t> application_hwm_bytes;
    application_hwm_profile_t application_hwm_profile =
      application_hwm_profile_t::balanced;
    std::optional<std::uint64_t> process_memory_limit_bytes;
    bool applied = false;

    void add_zlink_action (std::function<void (zlink_builder_t &)> action)
    {
        deferred_zlink_actions.push_back (std::move (action));
    }

    void set_zlink_action (std::string key, std::function<void (zlink_builder_t &)> action)
    {
        keyed_zlink_actions[std::move (key)] = std::move (action);
    }

};

} // namespace detail

} // namespace zlink::framework
