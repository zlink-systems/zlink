/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/locations/values.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <vector>

namespace zlink::framework
{

namespace detail
{
struct mesh_node_builder_state_t;
class mesh_node_runtime_t;
using mesh_handler_invoker_t =
  std::function<task_t<zlink::message_t> (service_provider_t &,
                                          serializer_registry_t &,
                                          const zlink::message_t &,
                                          const route_message_context_t &)>;
struct mesh_handler_registration_t
{
    bool request = false;
    std::string dispatch_name;
    std::string packet_name;
    std::type_index owner_type{typeid (void)};
    std::type_index message_type{typeid (void)};
    std::type_index reply_type{typeid (void)};
    mesh_handler_invoker_t invoke;
};
} // namespace detail

struct mesh_peer_connection_t
{
    std::uint64_t intent_id = 0;
    std::optional<zlink::routing_id_t> expected_routing_id;
    std::string endpoint;
};

class mesh_peer_connections_t
{
  public:
    void connect (std::string endpoint);
    void connect (zlink::routing_id_t expected_routing_id, std::string endpoint);
    void disconnect (std::string endpoint);
    std::vector<mesh_peer_connection_t> list_connections () const;

  private:
    friend class mesh_node_builder_t;
    explicit mesh_peer_connections_t (std::shared_ptr<detail::mesh_node_builder_state_t> state);
    std::shared_ptr<detail::mesh_node_builder_state_t> _state;
};

class mesh_channel_server_builder_t;

class mesh_channel_client_builder_t
{
};

class mesh_channel_builder_t
{
  public:
    mesh_channel_client_builder_t client ();
    mesh_channel_server_builder_t server ();

  private:
    friend class mesh_node_builder_t;
    mesh_channel_builder_t (std::shared_ptr<detail::mesh_node_builder_state_t> state,
                            std::string channel_name);
    std::shared_ptr<detail::mesh_node_builder_state_t> _state;
    std::string _channel_name;
};

class mesh_channel_server_builder_t
{
  public:
    mesh_channel_server_builder_t &set_weight (int weight);
    mesh_channel_server_builder_t &use_handler_group (std::string group_name);

    template <typename THandler, typename TMessage>
    mesh_channel_server_builder_t &add_send_handler (std::string packet_name = {})
    {
        const auto packet = packet_name.empty () ? detail::message_name<TMessage> ()
                                                 : std::move (packet_name);
        return add_handler<THandler, TMessage> (false, std::move (packet));
    }

    template <typename THandler, typename TRequest, typename TReply>
    mesh_channel_server_builder_t &add_request_handler (std::string packet_name = {})
    {
        const auto packet = packet_name.empty () ? detail::message_name<TRequest> ()
                                                 : std::move (packet_name);
        return add_handler<THandler, TRequest, TReply> (true, std::move (packet));
    }

  private:
    friend class mesh_channel_builder_t;
    friend class mesh_node_builder_t;
    mesh_channel_server_builder_t (
      std::shared_ptr<detail::mesh_node_builder_state_t> state,
      std::string channel_name);
    template <typename THandler, typename TMessage>
    mesh_channel_server_builder_t &add_handler (bool request, std::string packet_name);
    template <typename THandler, typename TRequest, typename TReply>
    mesh_channel_server_builder_t &add_handler (bool request, std::string packet_name);
    mesh_channel_server_builder_t &
    add_handler_registration (detail::mesh_handler_registration_t registration);
    std::shared_ptr<detail::mesh_node_builder_state_t> _state;
    std::string _channel_name;
};

struct mesh_node_socket_config_t
{
    std::int64_t max_message_size = 16 * 1024 * 1024;
    zlink::byte_count_t send_high_water_mark =
      zlink::byte_count_t::bytes (4'096'000);
    zlink::byte_count_t receive_high_water_mark =
      zlink::byte_count_t::bytes (4'096'000);
    std::uint64_t mailbox_message_budget = 1024;
    std::uint64_t mailbox_byte_budget = 64 * 1024 * 1024;
    std::optional<std::chrono::milliseconds> receive_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
};

class mesh_node_builder_t
{
  public:
    mesh_channel_builder_t channel_name (std::string channel_name);
    mesh_node_builder_t &listen (std::string endpoint);
    mesh_node_builder_t &set_advertise_host (std::string host);
    mesh_node_builder_t &set_routing_id (zlink::routing_id_t routing_id);
    mesh_node_builder_t &set_object_role (object_role_t role);
    mesh_node_builder_t &set_placement_weight (int weight);
    mesh_node_builder_t &set_actor_limit (std::int32_t limit);
    mesh_node_builder_t &set_spot_limit (std::int32_t limit);
    mesh_node_builder_t &set_instance_spot_idle_timeout (
      std::chrono::milliseconds timeout);
    mesh_node_builder_t &
    set_activation_concurrency (std::int32_t limit);
    mesh_node_socket_config_t &configure_router_socket ();
    mesh_peer_connections_t &peer_connections ();
    mesh_node_builder_t &set_default_request_timeout (std::chrono::milliseconds timeout);

    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_route_send_handler (std::string packet_name = {})
    {
        const auto packet = packet_name.empty () ? detail::message_name<TMessage> ()
                                                 : std::move (packet_name);
        return add_handler<THandler, TMessage> (false, std::move (packet));
    }

    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_route_request_handler (std::string packet_name = {})
    {
        const auto packet = packet_name.empty () ? detail::message_name<TRequest> ()
                                                 : std::move (packet_name);
        return add_handler<THandler, TRequest, TReply> (true, std::move (packet));
    }

    template <typename TEntrySpot>
    requires detail::entry_spot_type<TEntrySpot>
    mesh_node_builder_t &add_entry_spot (
      std::function<std::shared_ptr<TEntrySpot> (entry_spot_context_t)> factory)
    {
        spot_builder ().template add_entry_spot<TEntrySpot> (std::move (factory));
        return *this;
    }

    template <typename TSpot>
    requires detail::user_spot_type<TSpot>
    mesh_node_builder_t &add_spot_factory (
      std::string stable_type,
      std::function<std::shared_ptr<TSpot> (spot_context_t)> factory,
      std::function<void (user_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_spot_factory<TSpot> (
          std::move (stable_type), std::move (factory),
          std::move (configure));
        return *this;
    }

    template <typename TSpot>
    requires std::derived_from<TSpot, instance_spot_t>
    mesh_node_builder_t &add_instance_spot_factory (
      std::string stable_type,
      std::function<std::shared_ptr<TSpot> (instance_spot_context_t)> factory,
      std::function<void (instance_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_instance_spot_factory<TSpot> (
          std::move (stable_type), std::move (factory),
          std::move (configure));
        return *this;
    }

    template <typename TActor, typename TActorFactory>
    requires std::derived_from<TActor, actor_t>
             && std::derived_from<TActorFactory, actor_factory_t<TActor>>
    mesh_node_builder_t &
    add_actor_factory (std::string actor_type,
                       std::shared_ptr<TActorFactory> factory,
                       std::function<void (actor_factory_builder_t<TActor> &)> configure)
    {
        spot_builder ().template add_actor_factory<TActor, TActorFactory> (
          std::move (actor_type), std::move (factory),
          std::move (configure));
        return *this;
    }

  private:
    friend class zlink_builder_t;
    friend class zlink_framework_options_t;
    friend class detail::mesh_node_runtime_t;
    explicit mesh_node_builder_t (std::shared_ptr<detail::mesh_node_builder_state_t> state);
    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_handler (bool request, std::string packet_name);
    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_handler (bool request, std::string packet_name);
    void mark_node_direct_handler ();
    spot_node_builder_t &spot_builder ();
    std::string route_dispatch_name () const;
    std::shared_ptr<detail::mesh_node_builder_state_t> _state;
    mesh_peer_connections_t _peer_connections;
};

template <typename THandler, typename TMessage>
mesh_channel_server_builder_t &
mesh_channel_server_builder_t::add_handler (bool request, std::string packet_name)
{
    static_assert (!std::is_same_v<TMessage, void>);
    return add_handler_registration (detail::mesh_handler_registration_t{
      request,
      _channel_name,
      std::move (packet_name),
      std::type_index (typeid (THandler)),
      std::type_index (typeid (TMessage)),
      std::type_index (typeid (void)),
      [] (service_provider_t &services,
          serializer_registry_t &serializers,
          const zlink::message_t &message,
          const route_message_context_t &context) -> task_t<zlink::message_t> {
          try {
              auto &owner = services.get_required<THandler> ();
              auto payload = serializers.get<TMessage> ().deserialize (
                detail::encoded_payload_from_raw (message));
              if constexpr (requires {
                                static_cast<void (THandler::*) (
                                  const TMessage &, const route_message_context_t &)> (
                                  &THandler::handle);
                            }) {
                  (owner.*static_cast<void (THandler::*) (
                    const TMessage &, const route_message_context_t &)> (&THandler::handle)) (
                    payload, context);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              } else {
                  co_await (owner.*static_cast<task_t<void> (THandler::*) (
                    const TMessage &, const route_message_context_t &)> (&THandler::handle)) (
                    payload, context);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              }
          }
          catch (...) {
              co_return detail::current_exception_to_message_result (
                "MeshNode send handler threw an exception");
          }
      }});
}

template <typename THandler, typename TRequest, typename TReply>
mesh_channel_server_builder_t &
mesh_channel_server_builder_t::add_handler (bool request, std::string packet_name)
{
    return add_handler_registration (detail::mesh_handler_registration_t{
      request,
      _channel_name,
      std::move (packet_name),
      std::type_index (typeid (THandler)),
      std::type_index (typeid (TRequest)),
      std::type_index (typeid (TReply)),
      [] (service_provider_t &services,
          serializer_registry_t &serializers,
          const zlink::message_t &message,
          const route_message_context_t &context) -> task_t<zlink::message_t> {
          try {
              auto &owner = services.get_required<THandler> ();
              auto payload = serializers.get<TRequest> ().deserialize (
                detail::encoded_payload_from_raw (message));
              if constexpr (requires {
                                static_cast<TReply (THandler::*) (
                                  const TRequest &, const route_message_context_t &)> (
                                  &THandler::handle);
                            }) {
                  auto reply = (owner.*static_cast<TReply (THandler::*) (
                    const TRequest &, const route_message_context_t &)> (&THandler::handle)) (
                    payload, context);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (
                      serializers.get<TReply> ().serialize (reply)));
              } else {
                  auto reply = co_await (owner.*static_cast<task_t<TReply> (THandler::*) (
                    const TRequest &, const route_message_context_t &)> (&THandler::handle)) (
                    payload, context);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (
                      serializers.get<TReply> ().serialize (reply)));
              }
          }
          catch (...) {
              co_return detail::current_exception_to_message_result (
                "MeshNode request handler threw an exception");
          }
      }});
}

template <typename THandler, typename TMessage>
mesh_node_builder_t &
mesh_node_builder_t::add_handler (bool request, std::string packet_name)
{
    mark_node_direct_handler ();
    mesh_channel_server_builder_t route (_state, route_dispatch_name ());
    route.add_handler<THandler, TMessage> (request, std::move (packet_name));
    return *this;
}

template <typename THandler, typename TRequest, typename TReply>
mesh_node_builder_t &
mesh_node_builder_t::add_handler (bool request, std::string packet_name)
{
    mark_node_direct_handler ();
    mesh_channel_server_builder_t route (_state, route_dispatch_name ());
    route.add_handler<THandler, TRequest, TReply> (request, std::move (packet_name));
    return *this;
}

} // namespace zlink::framework
