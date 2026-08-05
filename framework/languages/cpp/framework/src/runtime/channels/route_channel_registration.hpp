/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/channels/route_handler_registry.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

class route_channel_registration_t
{
  public:
    explicit route_channel_registration_t (std::string router_channel_id);

    const std::string &router_channel_id () const noexcept;
    route_channel_registration_t &bind (std::string endpoint);
    route_channel_registration_t &set_routing_id (zlink::routing_id_t routing_id);
    route_channel_registration_t &connect (std::string endpoint);
    route_channel_registration_t &connect (zlink::routing_id_t peer_rid, std::string endpoint);
    route_channel_registration_t &default_request_timeout (std::chrono::milliseconds timeout);
    route_channel_registration_t &add_handler_group (std::string group_name);
    route_channel_registration_t &
    add_handler (framework::route_handler_registration_t registration);

    template <typename TOwner, typename TMessage>
    route_channel_registration_t &add_send_handler (
      std::string packet_name,
      void (TOwner::*method) (const TMessage &, const framework::route_message_context_t &))
    {
        _send_handlers.push_back (
          [packet = std::move (packet_name), method] (route_handler_registry_t &handlers,
                                                      const std::string &router_channel_id) {
              handlers.on_send<TOwner, TMessage> (router_channel_id, packet, method);
          });
        return *this;
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_channel_registration_t &add_request_handler (
      std::string packet_name,
      TReply (TOwner::*method) (const TRequest &, const framework::route_message_context_t &))
    {
        _request_handlers.push_back (
          [packet = std::move (packet_name), method] (route_handler_registry_t &handlers,
                                                      const std::string &router_channel_id) {
              handlers.on_request<TOwner, TRequest, TReply> (router_channel_id, packet, method);
          });
        return *this;
    }

    const std::string &bind_endpoint () const noexcept;
    const std::optional<zlink::routing_id_t> &routing_id () const noexcept;
    std::optional<std::chrono::milliseconds> default_request_timeout () const noexcept;
    const std::vector<std::string> &manual_connections () const noexcept;
    const std::vector<route_connection_set_t::target_t> &
    manual_connection_targets () const noexcept;
    const std::vector<std::string> &handler_groups () const noexcept;

    route_handler_registry_t create_handler_registry () const;

  private:
    using handler_installer_t =
      std::function<void (route_handler_registry_t &, const std::string &)>;

    std::string _router_channel_id;
    std::string _bind_endpoint;
    std::optional<zlink::routing_id_t> _routing_id;
    std::optional<std::chrono::milliseconds> _default_request_timeout;
    std::vector<std::string> _manual_connections;
    std::vector<route_connection_set_t::target_t> _manual_connection_targets;
    std::vector<std::string> _handler_groups;
    std::vector<framework::route_handler_registration_t> _handlers;
    std::vector<handler_installer_t> _send_handlers;
    std::vector<handler_installer_t> _request_handlers;
};

struct initialized_route_channel_t
{
    std::shared_ptr<route_channel_runtime_t> runtime;
    route_handler_registry_t handlers;
};

class route_channel_initializer_t
{
  public:
    initialized_route_channel_t initialize (const route_channel_registration_t &registration) const;
};

} // namespace zlink::framework::detail
