/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_channel_registration.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace zlink::framework::detail
{
namespace
{

bool is_blank (const std::string &value)
{
    return std::all_of (value.begin (), value.end (),
                        [] (unsigned char ch) { return std::isspace (ch) != 0; });
}

} // namespace

route_channel_registration_t::route_channel_registration_t (std::string router_channel_id) :
    _router_channel_id (std::move (router_channel_id))
{
    if (_router_channel_id.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel id is required");
    }
}

const std::string &route_channel_registration_t::router_channel_id () const noexcept
{
    return _router_channel_id;
}

route_channel_registration_t &route_channel_registration_t::bind (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel bind endpoint is required");
    }
    _bind_endpoint = std::move (endpoint);
    return *this;
}

route_channel_registration_t &
route_channel_registration_t::set_routing_id (zlink::routing_id_t routing_id)
{
    _routing_id = std::move (routing_id);
    return *this;
}

route_channel_registration_t &route_channel_registration_t::connect (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel manual connection endpoint is required");
    }
    _manual_connections.push_back (std::move (endpoint));
    return *this;
}

route_channel_registration_t &
route_channel_registration_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    if (peer_rid.size () == 0u) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel manual peer routing id is required");
    }
    if (endpoint.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel manual connection endpoint is required");
    }
    _manual_connection_targets.push_back (
      route_connection_set_t::target_t{std::move (endpoint), std::move (peer_rid)});
    return *this;
}

route_channel_registration_t &
route_channel_registration_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route channel request timeout must be greater than zero");
    }
    _default_request_timeout = timeout;
    return *this;
}

route_channel_registration_t &
route_channel_registration_t::add_handler_group (std::string group_name)
{
    if (group_name.empty () || is_blank (group_name)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route handler group name is required");
    }
    _handler_groups.push_back (std::move (group_name));
    return *this;
}

route_channel_registration_t &
route_channel_registration_t::add_handler (framework::route_handler_registration_t registration)
{
    if (registration.packet_name.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "route handler packet name is required");
    }
    _handlers.push_back (std::move (registration));
    return *this;
}

const std::string &route_channel_registration_t::bind_endpoint () const noexcept
{
    return _bind_endpoint;
}

const std::optional<zlink::routing_id_t> &route_channel_registration_t::routing_id () const noexcept
{
    return _routing_id;
}

std::optional<std::chrono::milliseconds>
route_channel_registration_t::default_request_timeout () const noexcept
{
    return _default_request_timeout;
}

const std::vector<std::string> &route_channel_registration_t::manual_connections () const noexcept
{
    return _manual_connections;
}

const std::vector<route_connection_set_t::target_t> &
route_channel_registration_t::manual_connection_targets () const noexcept
{
    return _manual_connection_targets;
}

const std::vector<std::string> &route_channel_registration_t::handler_groups () const noexcept
{
    return _handler_groups;
}

route_handler_registry_t route_channel_registration_t::create_handler_registry () const
{
    route_handler_registry_t handlers;
    for (const auto &handler : _handlers) {
        runtime::messaging::message_kind_t kind =
          handler.kind == framework::route_handler_kind_t::send
            ? runtime::messaging::message_kind_t::command
            : runtime::messaging::message_kind_t::request;
        handlers.add_handler (route_handler_descriptor_t{kind, _router_channel_id,
                                                         handler.packet_name, handler.owner_type,
                                                         handler.message_type, handler.reply_type},
                              handler.invoker);
    }
    for (const auto &install : _send_handlers) {
        install (handlers, _router_channel_id);
    }
    for (const auto &install : _request_handlers) {
        install (handlers, _router_channel_id);
    }
    return handlers;
}

initialized_route_channel_t
route_channel_initializer_t::initialize (const route_channel_registration_t &registration) const
{
    auto runtime = std::make_shared<route_channel_runtime_t> (registration.router_channel_id ());
    if (!registration.bind_endpoint ().empty ()) {
        runtime->bind_endpoint (registration.bind_endpoint ());
        runtime->connect (registration.bind_endpoint ());
    }
    if (registration.routing_id ()) {
        runtime->routing_id (*registration.routing_id ());
    }
    if (registration.default_request_timeout ()) {
        runtime->default_request_timeout (*registration.default_request_timeout ());
    }
    for (const auto &endpoint : registration.manual_connections ()) {
        runtime->connect (endpoint);
    }
    for (const auto &target : registration.manual_connection_targets ()) {
        if (target.peer_rid) {
            runtime->connect (*target.peer_rid, target.endpoint);
        }
    }
    runtime->manual_connections (registration.manual_connections ());
    runtime->start ();
    return initialized_route_channel_t{std::move (runtime),
                                       registration.create_handler_registry ()};
}

} // namespace zlink::framework::detail
