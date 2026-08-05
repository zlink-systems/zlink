/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_runtime_manager.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include "runtime/channels/route_channel_registration.hpp"

#include <utility>

namespace zlink::framework::detail
{

namespace
{

bool enabled_for (const channel_snapshot_t &channel,
                  channel_runtime_manager_t::capability_t capability)
{
    switch (capability) {
        case channel_runtime_manager_t::capability_t::server:
            return channel.server.enabled;
        case channel_runtime_manager_t::capability_t::client:
            return channel.client.enabled;
        case channel_runtime_manager_t::capability_t::publisher:
            return channel.publisher.enabled;
        case channel_runtime_manager_t::capability_t::subscriber:
            return channel.subscriber.enabled;
    }
    return false;
}

} // namespace

channel_runtime_manager_t::channel_runtime_manager_t (
  std::shared_ptr<channel_runtime_state_t> state) :
    _state (std::move (state))
{
}

channel_runtime_manager_t channel_runtime_manager_t::from (const message_bus_t &bus)
{
    return channel_runtime_manager_t (bus._state);
}

channel_runtime_manager_t channel_runtime_manager_t::from (const zlink_builder_t &builder)
{
    return channel_runtime_manager_t (builder._state->runtime);
}

channel_runtime_bundle_t &
channel_runtime_manager_t::get_or_create_client_bundle (const std::string &channel_name)
{
    if (auto found = _state->client_bundles.find (channel_name);
        found != _state->client_bundles.end ()) {
        return *found->second;
    }
    const auto &channel = require_channel (channel_name, capability_t::client);
    auto bundle = _bundle_factory.create_client_bundle (channel_name, channel);
    auto &stored = _state->client_bundles[channel_name] = std::move (bundle);
    return *stored;
}

channel_runtime_bundle_t &
channel_runtime_manager_t::get_or_create_publisher_bundle (const std::string &channel_name)
{
    if (auto found = _state->publisher_bundles.find (channel_name);
        found != _state->publisher_bundles.end ()) {
        return *found->second;
    }
    const auto &channel = require_channel (channel_name, capability_t::publisher);
    auto bundle = _bundle_factory.create_publisher_bundle (channel_name, channel);
    auto &stored = _state->publisher_bundles[channel_name] = std::move (bundle);
    return *stored;
}

channel_runtime_bundle_t &
channel_runtime_manager_t::get_or_create_subscriber_bundle (const std::string &channel_name)
{
    if (auto found = _state->subscriber_bundles.find (channel_name);
        found != _state->subscriber_bundles.end ()) {
        return *found->second;
    }
    const auto &channel = require_channel (channel_name, capability_t::subscriber);
    auto bundle = _bundle_factory.create_subscriber_bundle (channel_name, channel);
    auto &stored = _state->subscriber_bundles[channel_name] = std::move (bundle);
    return *stored;
}

route_channel_runtime_t &
channel_runtime_manager_t::get_route_channel (const std::string &router_channel_id)
{
    if (auto found = _state->route_channels.find (router_channel_id);
        found != _state->route_channels.end ()) {
        return *found->second;
    }
    throw framework_exception_t (framework_error_kind_t::unavailable,
                                 "route channel '" + router_channel_id + "' is not registered");
}

const route_handler_registry_t &
channel_runtime_manager_t::get_route_handlers (const std::string &router_channel_id) const
{
    if (auto found = _state->route_handlers.find (router_channel_id);
        found != _state->route_handlers.end ()) {
        return found->second;
    }
    static const route_handler_registry_t empty;
    return empty;
}

std::vector<std::string> channel_runtime_manager_t::route_channel_ids () const
{
    std::vector<std::string> ids;
    ids.reserve (_state->route_channels.size ());
    for (const auto &[route_id, _] : _state->route_channels) {
        ids.push_back (route_id);
    }
    return ids;
}

std::vector<std::string> channel_runtime_manager_t::configured_route_channel_ids (
  const zlink_builder_t &builder)
{
    std::vector<std::string> ids;
    ids.reserve (builder._state->route_channels.size ());
    for (const auto &[route_id, _] : builder._state->route_channels) {
        ids.push_back (route_id);
    }
    return ids;
}

void channel_runtime_manager_t::initialize_inbound_channels ()
{
    for (const auto &[channel_name, channel] : _state->channels) {
        if (channel.server.enabled
            && _state->server_bundles.find (channel_name) == _state->server_bundles.end ()) {
            _state->server_bundles[channel_name] =
              _bundle_factory.create_server_bundle (channel_name, channel);
        }
        if (channel.subscriber.enabled
            && _state->subscriber_bundles.find (channel_name)
                 == _state->subscriber_bundles.end ()) {
            _state->subscriber_bundles[channel_name] =
              _bundle_factory.create_subscriber_bundle (channel_name, channel);
        }
    }
}

void channel_runtime_manager_t::initialize_publisher_channels ()
{
    for (const auto &[channel_name, channel] : _state->channels) {
        if (channel.publisher.enabled) {
            (void) get_or_create_publisher_bundle (channel_name);
        }
    }
}

void channel_runtime_manager_t::initialize_client_channels ()
{
    for (const auto &[channel_name, channel] : _state->channels) {
        if (channel.client.enabled) {
            (void) get_or_create_client_bundle (channel_name);
        }
    }
}

void channel_runtime_manager_t::initialize_route_channels (
  const std::vector<std::string> &route_ids)
{
    for (const auto &route_id : route_ids) {
        if (_state->route_channels.find (route_id) != _state->route_channels.end ()) {
            continue;
        }
        auto runtime = std::make_shared<route_channel_runtime_t> (route_id);
        runtime->start ();
        if (auto found = _state->channels.find (route_id); found != _state->channels.end ()) {
            for (const auto &endpoint : found->second.client.connect_endpoints) {
                runtime->connect (endpoint);
            }
            for (const auto &endpoint : found->second.server.bind_endpoints) {
                runtime->connect (endpoint);
            }
        }
        _state->route_channels.emplace (route_id, std::move (runtime));
    }
}

void channel_runtime_manager_t::initialize_route_channels (
  const std::map<std::string, std::shared_ptr<route_channel_builder_state_t>> &registrations)
{
    route_channel_initializer_t initializer;
    for (const auto &[route_id, registration] : registrations) {
        if (_state->route_channels.find (route_id) != _state->route_channels.end ()) {
            continue;
        }
        auto initialized = initializer.initialize (registration->registration);
        _state->route_handlers[route_id] = std::move (initialized.handlers);
        _state->route_channels.emplace (route_id, std::move (initialized.runtime));
    }
}

void channel_runtime_manager_t::initialize_route_channels (const zlink_builder_t &builder)
{
    initialize_route_channels (builder._state->route_channels);
}

std::string channel_runtime_manager_t::monitoring_source (const std::string &source_name)
{
    const auto [channel_name, capability] = parse_source (source_name);
    if (capability == "server"
        && _state->server_bundles.find (channel_name) != _state->server_bundles.end ()) {
        return source_name;
    }
    if (capability == "subscriber"
        && _state->subscriber_bundles.find (channel_name) != _state->subscriber_bundles.end ()) {
        return source_name;
    }
    if (capability == "publisher") {
        (void) get_or_create_publisher_bundle (channel_name);
        return source_name;
    }
    if (capability == "client") {
        (void) get_or_create_client_bundle (channel_name);
        return source_name;
    }
    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 "socket monitoring source '" + source_name
                                   + "' is not registered");
}

const channel_snapshot_t &
channel_runtime_manager_t::require_channel (const std::string &channel_name,
                                            capability_t capability) const
{
    const auto found = _state->channels.find (channel_name);
    if (found == _state->channels.end () || !enabled_for (found->second, capability)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "channel capability '" + channel_name + "' is not registered");
    }
    return found->second;
}

std::pair<std::string, std::string>
channel_runtime_manager_t::parse_source (const std::string &source_name)
{
    const auto separator = source_name.rfind ('.');
    if (separator == std::string::npos || separator == 0 || separator + 1 == source_name.size ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "socket monitoring source must use '<channel>.<capability>'");
    }
    return {source_name.substr (0, separator), source_name.substr (separator + 1)};
}

} // namespace zlink::framework::detail
