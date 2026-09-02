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
#include <concepts>
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

void bind_mesh_handler_services (std::shared_ptr<mesh_node_builder_state_t> state,
                                 service_collection_t &services);
void register_mesh_handler_service (std::shared_ptr<mesh_node_builder_state_t> state,
                                    std::function<void (service_collection_t &)> registrar);

template <typename THandler>
void register_mesh_handler_service_for (std::shared_ptr<mesh_node_builder_state_t> state)
{
    register_mesh_handler_service (std::move (state), [] (service_collection_t &services) {
        if (services.contains (std::type_index (typeid (THandler)))) {
            return;
        }
        injected_handler_registrar_t<THandler>::add (services);
    });
}
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
class mesh_object_role_builder_t;
class mesh_object_client_builder_t;
class mesh_object_server_builder_t;
template <typename TSpot, typename... TDependencies> class user_spot_registration_builder_t;
template <typename TSpot, typename... TDependencies> class instance_spot_registration_builder_t;
template <typename TActor, typename TActorFactory> class actor_registration_builder_t;

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
    mesh_channel_server_builder_t &add_handler_group (std::string group_name);

    template <typename THandler, typename TMessage>
    mesh_channel_server_builder_t &add_send_handler (std::string packet_name = {})
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TMessage> () : std::move (packet_name);
        return add_handler<THandler, TMessage> (false, std::move (packet));
    }

    template <typename THandler, typename TRequest, typename TReply>
    mesh_channel_server_builder_t &add_request_handler (std::string packet_name = {})
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TRequest> () : std::move (packet_name);
        return add_handler<THandler, TRequest, TReply> (true, std::move (packet));
    }

  private:
    friend class mesh_channel_builder_t;
    friend class mesh_node_builder_t;
    friend class detail::mesh_node_runtime_t;
    mesh_channel_server_builder_t (std::shared_ptr<detail::mesh_node_builder_state_t> state,
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
    std::optional<std::chrono::milliseconds> receive_timeout;
    std::optional<std::chrono::milliseconds> send_timeout;
};

class mesh_node_builder_t
{
  public:
    mesh_channel_builder_t channel (std::string channel_name);
    mesh_channel_builder_t channel_name (std::string channel_name);
    mesh_object_role_builder_t objects ();
    mesh_node_builder_t &listen (std::string endpoint);
    mesh_node_builder_t &listen (std::uint16_t port = 0);
    mesh_node_builder_t &set_bind_host (std::string host);
    mesh_node_builder_t &set_advertise_host (std::string host);
    mesh_node_builder_t &set_routing_id (zlink::routing_id_t routing_id);
    mesh_node_builder_t &set_automatic_routing_id_prefix (std::string prefix);
    mesh_node_builder_t &set_object_role (object_role_t role);
    mesh_node_builder_t &set_placement_weight (int weight);
    mesh_node_builder_t &set_actor_limit (std::int32_t limit);
    mesh_node_builder_t &set_spot_limit (std::int32_t limit);
    mesh_node_builder_t &set_instance_spot_idle_timeout (std::chrono::milliseconds timeout);
    mesh_node_builder_t &set_activation_concurrency (std::int32_t limit);
    mesh_node_socket_config_t &configure_router_socket ();
    mesh_peer_connections_t &peer_connections ();
    mesh_node_builder_t &set_default_request_timeout (std::chrono::milliseconds timeout);

    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_route_send_handler (std::string packet_name = {})
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TMessage> () : std::move (packet_name);
        return add_handler<THandler, TMessage> (false, std::move (packet));
    }

    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_route_request_handler (std::string packet_name = {})
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TRequest> () : std::move (packet_name);
        return add_handler<THandler, TRequest, TReply> (true, std::move (packet));
    }

    template <typename TEntrySpot, typename... TDependencies>
    requires detail::entry_spot_type<TEntrySpot> &&std::
      constructible_from<TEntrySpot, entry_spot_context_t, TDependencies &...> mesh_node_builder_t &
      add_entry_spot ()
    {
        spot_builder ().template add_entry_spot<TEntrySpot, TDependencies...> ();
        return *this;
    }

    template <typename TEntrySpot>
    requires detail::entry_spot_type<TEntrySpot> mesh_node_builder_t &
    add_entry_spot (std::function<std::shared_ptr<TEntrySpot> (entry_spot_context_t)> factory)
    {
        spot_builder ().template add_entry_spot<TEntrySpot> (std::move (factory));
        return *this;
    }

    template <typename TSpot>
    requires detail::user_spot_type<TSpot> mesh_node_builder_t &
    add_spot_factory (std::string stable_type,
                      std::function<std::shared_ptr<TSpot> (spot_context_t)> factory,
                      std::function<void (user_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_spot_factory<TSpot> (
          std::move (stable_type), std::move (factory), std::move (configure));
        return *this;
    }

    template <typename TSpot>
    requires std::derived_from<TSpot, instance_spot_t> mesh_node_builder_t &
    add_instance_spot_factory (
      std::string stable_type,
      std::function<std::shared_ptr<TSpot> (instance_spot_context_t)> factory,
      std::function<void (instance_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_instance_spot_factory<TSpot> (
          std::move (stable_type), std::move (factory), std::move (configure));
        return *this;
    }

    template <typename TActor, typename TActorFactory>
    requires std::derived_from<TActor, actor_t> &&
      std::derived_from<TActorFactory, actor_factory_t<TActor>> mesh_node_builder_t &
      add_actor_factory (std::string actor_type,
                         std::shared_ptr<TActorFactory> factory,
                         std::function<void (actor_factory_builder_t<TActor> &)> configure)
    {
        spot_builder ().template add_actor_factory<TActor, TActorFactory> (
          std::move (actor_type), std::move (factory), std::move (configure));
        return *this;
    }

  private:
    friend class zlink_builder_t;
    friend class zlink_framework_options_t;
    friend class detail::mesh_node_runtime_t;
    friend class mesh_object_role_builder_t;
    friend class mesh_object_client_builder_t;
    friend class mesh_object_server_builder_t;
    template <typename, typename...> friend class user_spot_registration_builder_t;
    template <typename, typename...> friend class instance_spot_registration_builder_t;
    template <typename, typename> friend class actor_registration_builder_t;
    explicit mesh_node_builder_t (std::shared_ptr<detail::mesh_node_builder_state_t> state);
    template <typename THandler, typename TMessage>
    mesh_node_builder_t &add_handler (bool request, std::string packet_name);
    template <typename THandler, typename TRequest, typename TReply>
    mesh_node_builder_t &add_handler (bool request, std::string packet_name);
    void mark_node_direct_handler ();
    spot_node_builder_t &spot_builder ();

    template <typename TSpot, typename... TDependencies>
    mesh_node_builder_t &
    add_injected_spot_factory (std::string stable_type,
                               std::function<void (user_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_spot_factory<TSpot, TDependencies...> (std::move (stable_type),
                                                                            std::move (configure));
        return *this;
    }

    template <typename TSpot, typename... TDependencies>
    mesh_node_builder_t &add_injected_instance_spot_factory (
      std::string stable_type,
      std::function<void (instance_spot_factory_builder_t<TSpot> &)> configure)
    {
        spot_builder ().template add_instance_spot_factory<TSpot, TDependencies...> (
          std::move (stable_type), std::move (configure));
        return *this;
    }
    std::string route_dispatch_name () const;
    std::shared_ptr<detail::mesh_node_builder_state_t> _state;
    mesh_peer_connections_t _peer_connections;
};

class mesh_object_client_builder_t
{
  public:
    explicit mesh_object_client_builder_t (mesh_node_builder_t node) : _node (std::move (node)) {}

  private:
    mesh_node_builder_t _node;
};

class mesh_object_server_builder_t
{
  public:
    explicit mesh_object_server_builder_t (mesh_node_builder_t node) : _node (std::move (node)) {}

    template <typename TEntrySpot, typename... TDependencies>
    mesh_object_server_builder_t &add_entry_spot ()
    {
        _node.template add_entry_spot<TEntrySpot, TDependencies...> ();
        return *this;
    }

    template <typename TSpot, typename... TDependencies>
    [[nodiscard]] user_spot_registration_builder_t<TSpot, TDependencies...>
    add_spot_factory (std::string stable_type)
    {
        return user_spot_registration_builder_t<TSpot, TDependencies...> (*this,
                                                                          std::move (stable_type));
    }

    template <typename TSpot, typename... TDependencies>
    [[nodiscard]] instance_spot_registration_builder_t<TSpot, TDependencies...>
    add_instance_spot_factory (std::string stable_type)
    {
        return instance_spot_registration_builder_t<TSpot, TDependencies...> (
          *this, std::move (stable_type));
    }

    template <typename TActor, typename TActorFactory>
    [[nodiscard]] actor_registration_builder_t<TActor, TActorFactory>
    add_actor_factory (std::string stable_type)
    {
        return actor_registration_builder_t<TActor, TActorFactory> (*this, std::move (stable_type));
    }

  private:
    template <typename, typename...> friend class user_spot_registration_builder_t;
    template <typename, typename...> friend class instance_spot_registration_builder_t;
    template <typename, typename> friend class actor_registration_builder_t;

    mesh_node_builder_t _node;
};

class mesh_object_role_builder_t
{
  public:
    explicit mesh_object_role_builder_t (mesh_node_builder_t node) : _node (std::move (node)) {}

    mesh_object_client_builder_t client ()
    {
        _node.set_object_role (object_role_t::client);
        return mesh_object_client_builder_t (_node);
    }

    mesh_object_server_builder_t server ()
    {
        _node.set_object_role (object_role_t::server);
        return mesh_object_server_builder_t (_node);
    }

  private:
    mesh_node_builder_t _node;
};

template <typename TSpot, typename... TDependencies> class user_spot_registration_builder_t
{
  public:
    user_spot_registration_builder_t (mesh_object_server_builder_t server,
                                      std::string stable_type) :
        _server (std::move (server)), _stable_type (std::move (stable_type))
    {
    }

    user_spot_registration_builder_t &set_stable_type_limit (std::int32_t value)
    {
        _stable_type_limit = value;
        return *this;
    }

    user_spot_registration_builder_t &set_execution_mode (user_spot_execution_mode_t value)
    {
        _execution_mode = value;
        return *this;
    }

    user_spot_registration_builder_t &
    set_relocation_coordination_mode (spot_relocation_coordination_mode_t value)
    {
        _relocation_coordination_mode = value;
        return *this;
    }

    mesh_object_server_builder_t disable_relocation ()
    {
        return commit ([] (auto &factory) { factory.disable_relocation (); });
    }

    mesh_object_server_builder_t recreate_on_relocation ()
    {
        return commit ([] (auto &factory) { factory.recreate_on_relocation (); });
    }

    template <typename TAdapter> mesh_object_server_builder_t preserve_state_with ()
    {
        return commit ([] (user_spot_factory_builder_t<TSpot> &factory) {
            factory.template preserve_state_with<TAdapter> ();
        });
    }

  private:
    template <typename TPolicy> mesh_object_server_builder_t commit (TPolicy policy)
    {
        const auto stable_type_limit = _stable_type_limit;
        const auto execution_mode = _execution_mode;
        const auto relocation_coordination_mode = _relocation_coordination_mode;
        _server._node.template add_injected_spot_factory<TSpot, TDependencies...> (
          std::move (_stable_type),
          [stable_type_limit, execution_mode, relocation_coordination_mode,
           policy = std::move (policy)] (user_spot_factory_builder_t<TSpot> &factory) mutable {
              if (stable_type_limit) {
                  factory.set_stable_type_limit (*stable_type_limit);
              }
              if (execution_mode) {
                  factory.set_execution_mode (*execution_mode);
              }
              if (relocation_coordination_mode) {
                  factory.set_relocation_coordination_mode (*relocation_coordination_mode);
              }
              policy (factory);
          });
        return _server;
    }

    mesh_object_server_builder_t _server;
    std::string _stable_type;
    std::optional<std::int32_t> _stable_type_limit;
    std::optional<user_spot_execution_mode_t> _execution_mode;
    std::optional<spot_relocation_coordination_mode_t> _relocation_coordination_mode;
};

template <typename TSpot, typename... TDependencies> class instance_spot_registration_builder_t
{
  public:
    instance_spot_registration_builder_t (mesh_object_server_builder_t server,
                                          std::string stable_type) :
        _server (std::move (server)), _stable_type (std::move (stable_type))
    {
    }

    instance_spot_registration_builder_t &set_stable_type_limit (std::int32_t value)
    {
        _stable_type_limit = value;
        return *this;
    }

    mesh_object_server_builder_t disable_relocation ()
    {
        return commit ([] (auto &factory) { factory.disable_relocation (); });
    }

    mesh_object_server_builder_t recreate_on_relocation ()
    {
        return commit ([] (auto &factory) { factory.recreate_on_relocation (); });
    }

    template <typename TAdapter> mesh_object_server_builder_t preserve_state_with ()
    {
        return commit ([] (instance_spot_factory_builder_t<TSpot> &factory) {
            factory.template preserve_state_with<TAdapter> ();
        });
    }

  private:
    template <typename TPolicy> mesh_object_server_builder_t commit (TPolicy policy)
    {
        const auto stable_type_limit = _stable_type_limit;
        _server._node.template add_injected_instance_spot_factory<TSpot, TDependencies...> (
          std::move (_stable_type), [stable_type_limit, policy = std::move (policy)] (
                                      instance_spot_factory_builder_t<TSpot> &factory) mutable {
              if (stable_type_limit) {
                  factory.set_stable_type_limit (*stable_type_limit);
              }
              policy (factory);
          });
        return _server;
    }

    mesh_object_server_builder_t _server;
    std::string _stable_type;
    std::optional<std::int32_t> _stable_type_limit;
};

template <typename TActor, typename TActorFactory> class actor_registration_builder_t
{
  public:
    actor_registration_builder_t (mesh_object_server_builder_t server, std::string stable_type) :
        _server (std::move (server)), _stable_type (std::move (stable_type))
    {
        static_assert (std::default_initializable<TActorFactory>,
                       "Actor factory must be default constructible");
    }

    mesh_object_server_builder_t disable_relocation ()
    {
        return commit ([] (auto &factory) { factory.disable_relocation (); });
    }

    mesh_object_server_builder_t recreate_on_relocation ()
    {
        return commit ([] (auto &factory) { factory.recreate_on_relocation (); });
    }

    template <typename TAdapter> mesh_object_server_builder_t preserve_state_with ()
    {
        return commit ([] (actor_factory_builder_t<TActor> &factory) {
            factory.template preserve_state_with<TAdapter> ();
        });
    }

  private:
    template <typename TPolicy> mesh_object_server_builder_t commit (TPolicy policy)
    {
        _server._node.template add_actor_factory<TActor, TActorFactory> (
          std::move (_stable_type), std::make_shared<TActorFactory> (),
          [policy = std::move (policy)] (actor_factory_builder_t<TActor> &factory) mutable {
              policy (factory);
          });
        return _server;
    }

    mesh_object_server_builder_t _server;
    std::string _stable_type;
};

inline mesh_object_role_builder_t mesh_node_builder_t::objects ()
{
    return mesh_object_role_builder_t (*this);
}

template <typename THandler, typename TMessage>
mesh_channel_server_builder_t &mesh_channel_server_builder_t::add_handler (bool request,
                                                                           std::string packet_name)
{
    static_assert (!std::is_same_v<TMessage, void>);
    detail::register_mesh_handler_service_for<THandler> (_state);
    return add_handler_registration (detail::mesh_handler_registration_t{
      request, _channel_name, std::move (packet_name), std::type_index (typeid (THandler)),
      std::type_index (typeid (TMessage)), std::type_index (typeid (void)),
      [] (service_provider_t &services, serializer_registry_t &serializers,
          const zlink::message_t &message,
          const route_message_context_t &context) -> task_t<zlink::message_t> {
          try {
              auto &owner = services.get_required<THandler> ();
              auto payload = detail::deserialize_typed_payload<TMessage> (
                serializers, message, context.content_type.value_or (""));
              if constexpr (requires {
                                static_cast<void (THandler::*) (const TMessage &,
                                                                const route_message_context_t &)> (
                                  &THandler::handle);
                            }) {
                  (owner
                   .*static_cast<void (THandler::*) (
                     const TMessage &, const route_message_context_t &)> (&THandler::handle)) (
                    payload, context);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              } else if constexpr (requires {
                                       static_cast<task_t<void> (THandler::*) (
                                         const TMessage &, const route_message_context_t &)> (
                                         &THandler::handle);
                                   }) {
                  co_await (owner
                            .*static_cast<task_t<void> (THandler::*) (
                              const TMessage &, const route_message_context_t &)> (
                              &THandler::handle)) (payload, context);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              } else if constexpr (requires {
                                       static_cast<void (THandler::*) (const TMessage &)> (
                                         &THandler::handle);
                                   }) {
                  (owner.*static_cast<void (THandler::*) (const TMessage &)> (&THandler::handle)) (
                    payload);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              } else {
                  co_await (owner
                            .*static_cast<task_t<void> (THandler::*) (const TMessage &)> (
                              &THandler::handle)) (payload);
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
mesh_channel_server_builder_t &mesh_channel_server_builder_t::add_handler (bool request,
                                                                           std::string packet_name)
{
    detail::register_mesh_handler_service_for<THandler> (_state);
    return add_handler_registration (detail::mesh_handler_registration_t{
      request, _channel_name, std::move (packet_name), std::type_index (typeid (THandler)),
      std::type_index (typeid (TRequest)), std::type_index (typeid (TReply)),
      [] (service_provider_t &services, serializer_registry_t &serializers,
          const zlink::message_t &message,
          const route_message_context_t &context) -> task_t<zlink::message_t> {
          try {
              auto &owner = services.get_required<THandler> ();
              auto payload = detail::deserialize_typed_payload<TRequest> (
                serializers, message, context.content_type.value_or (""));
              if constexpr (requires {
                                static_cast<TReply (THandler::*) (
                                  const TRequest &, const route_message_context_t &)> (
                                  &THandler::handle);
                            }) {
                  auto reply = (owner
                                .*static_cast<TReply (THandler::*) (
                                  const TRequest &, const route_message_context_t &)> (
                                  &THandler::handle)) (payload, context);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              } else if constexpr (requires {
                                       static_cast<task_t<TReply> (THandler::*) (
                                         const TRequest &, const route_message_context_t &)> (
                                         &THandler::handle);
                                   }) {
                  auto reply = co_await (owner
                                         .*static_cast<task_t<TReply> (THandler::*) (
                                           const TRequest &, const route_message_context_t &)> (
                                           &THandler::handle)) (payload, context);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              } else if constexpr (requires {
                                       static_cast<TReply (THandler::*) (const TRequest &)> (
                                         &THandler::handle);
                                   }) {
                  auto reply = (owner
                                .*static_cast<TReply (THandler::*) (const TRequest &)> (
                                  &THandler::handle)) (payload);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              } else {
                  auto reply =
                    co_await (owner
                              .*static_cast<task_t<TReply> (THandler::*) (const TRequest &)> (
                                &THandler::handle)) (payload);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              }
          }
          catch (...) {
              co_return detail::current_exception_to_message_result (
                "MeshNode request handler threw an exception");
          }
      }});
}

template <typename THandler, typename TMessage>
mesh_node_builder_t &mesh_node_builder_t::add_handler (bool request, std::string packet_name)
{
    mark_node_direct_handler ();
    mesh_channel_server_builder_t route (_state, route_dispatch_name ());
    route.add_handler<THandler, TMessage> (request, std::move (packet_name));
    return *this;
}

template <typename THandler, typename TRequest, typename TReply>
mesh_node_builder_t &mesh_node_builder_t::add_handler (bool request, std::string packet_name)
{
    mark_node_direct_handler ();
    mesh_channel_server_builder_t route (_state, route_dispatch_name ());
    route.add_handler<THandler, TRequest, TReply> (request, std::move (packet_name));
    return *this;
}

} // namespace zlink::framework
