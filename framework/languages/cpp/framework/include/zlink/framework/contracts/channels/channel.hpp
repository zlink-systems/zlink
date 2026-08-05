/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/error.hpp>
#include <zlink/framework/contracts/messaging/message_context.hpp>
#include <zlink/framework/contracts/spots/spot_identity.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework
{

class route_channel_builder_t;

namespace detail
{
class capability_builder_state_t;
class channel_builder_state_t;
class channel_runtime_state_t;
class channel_runtime_t;
class channel_outbound_exchange_t;
class channel_runtime_manager_t;
class route_client_state_t;
class route_client_runtime_t;
class route_channel_builder_state_t;
void connect_route_channel_peer (route_channel_builder_t &builder,
                                 zlink::routing_id_t peer_rid,
                                 std::string endpoint);
} // namespace detail

class spot_context_t;
class channel_runtime_options_t;
class client_server_channel_runtime_options_t;
class route_mesh_channel_runtime_options_t;
class channel_server_socket_runtime_options_t;
enum class channel_capability_t
{
    server = 0,
    client = 1,
    publisher = 2,
    subscriber = 3
};

struct channel_capability_snapshot_t
{
    bool enabled = false;
    bool discovery = false;
    std::optional<zlink::routing_id_t> routing_id;
    std::optional<zlink::byte_count_t> send_high_water_mark;
    std::optional<zlink::byte_count_t> receive_high_water_mark;
    std::optional<zlink::byte_size_t> max_message_size =
      zlink::byte_size_t::bytes (16 * 1024 * 1024);
    std::optional<zlink::peer_weight_t> peer_weight;
    int service_weight = 100;
    std::vector<std::string> bind_endpoints;
    std::vector<std::string> connect_endpoints;
};

struct channel_snapshot_t
{
    std::string name;
    std::optional<std::chrono::milliseconds> default_request_timeout;
    channel_capability_snapshot_t server;
    channel_capability_snapshot_t client;
    channel_capability_snapshot_t publisher;
    channel_capability_snapshot_t subscriber;
};

enum class route_handler_kind_t
{
    send = 0,
    request = 1
};

struct route_handler_registration_t
{
    route_handler_kind_t kind;
    std::string packet_name;
    std::type_index owner_type;
    std::type_index message_type;
    std::type_index reply_type;
    std::function<task_t<zlink::message_t> (service_provider_t &,
                                            serializer_registry_t &,
                                            const zlink::message_t &,
                                            const route_message_context_t &)>
      invoker;
};

class capability_builder_t
{
  public:
    capability_builder_t ();
    ~capability_builder_t ();

    capability_builder_t (capability_builder_t &&) noexcept;
    capability_builder_t &operator= (capability_builder_t &&) noexcept;
    capability_builder_t (const capability_builder_t &) = default;
    capability_builder_t &operator= (const capability_builder_t &) = default;

    capability_builder_t &bind (std::string endpoint);
    capability_builder_t &connect (std::string endpoint);
    capability_builder_t &set_routing_id (zlink::routing_id_t routing_id);
    capability_builder_t &send_high_water_mark (zlink::byte_count_t value);
    capability_builder_t &receive_high_water_mark (zlink::byte_count_t value);
    capability_builder_t &max_message_size (zlink::byte_size_t value);
    capability_builder_t &peer_weight (zlink::peer_weight_t value);
    capability_builder_t &service_weight (int value);

    channel_capability_snapshot_t snapshot () const;

  private:
    friend class channel_builder_t;
    explicit capability_builder_t (std::shared_ptr<detail::capability_builder_state_t> state);

    std::shared_ptr<detail::capability_builder_state_t> _state;
};

class channel_builder_t
{
  public:
    channel_builder_t ();
    ~channel_builder_t ();

    channel_builder_t (channel_builder_t &&) noexcept;
    channel_builder_t &operator= (channel_builder_t &&) noexcept;
    channel_builder_t (const channel_builder_t &) = default;
    channel_builder_t &operator= (const channel_builder_t &) = default;

    capability_builder_t enable_server ();
    capability_builder_t enable_client ();
    capability_builder_t enable_publisher ();
    capability_builder_t enable_subscriber ();
    channel_builder_t &default_request_timeout (std::chrono::milliseconds timeout);

    channel_snapshot_t snapshot () const;

  private:
    friend class zlink_builder_t;
    explicit channel_builder_t (std::shared_ptr<detail::channel_builder_state_t> state);
    capability_builder_t enable_capability (channel_capability_snapshot_t &target);

    std::shared_ptr<detail::channel_builder_state_t> _state;
};

class route_channel_builder_t
{
  public:
    route_channel_builder_t ();
    ~route_channel_builder_t ();

    route_channel_builder_t (route_channel_builder_t &&) noexcept;
    route_channel_builder_t &operator= (route_channel_builder_t &&) noexcept;
    route_channel_builder_t (const route_channel_builder_t &) = default;
    route_channel_builder_t &operator= (const route_channel_builder_t &) = default;

    route_channel_builder_t &bind (std::string endpoint);
    route_channel_builder_t &set_routing_id (zlink::routing_id_t routing_id);
    route_channel_builder_t &connect (std::string endpoint);
    route_channel_builder_t &default_request_timeout (std::chrono::milliseconds timeout);
    route_channel_builder_t &add_handler_group (std::string group_name);

    template <typename TOwner, typename TMessage>
    route_channel_builder_t &
    add_send_handler (std::string packet_name,
                      void (TOwner::*method) (const TMessage &, const route_message_context_t &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TMessage> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::send, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TMessage)), std::type_index (typeid (void)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &context) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TMessage> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  (owner.*method) (payload, context);
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (zlink::message_t{}));
              }
              catch (const framework_exception_t &error) {
                  return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                         "routed send handler threw an exception"));
              }
          }});
    }

    template <typename TOwner, typename TMessage>
    route_channel_builder_t &add_send_handler (std::string packet_name,
                                               void (TOwner::*method) (const TMessage &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TMessage> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::send, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TMessage)), std::type_index (typeid (void)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TMessage> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  (owner.*method) (payload);
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (zlink::message_t{}));
              }
              catch (const framework_exception_t &error) {
                  return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                         "routed send handler threw an exception"));
              }
          }});
    }

    template <typename TOwner, typename TMessage>
    route_channel_builder_t &add_send_handler (std::string packet_name,
                                               task_t<void> (TOwner::*method) (const TMessage &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TMessage> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::send, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TMessage)), std::type_index (typeid (void)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TMessage> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  co_await (owner.*method) (payload);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              }
              catch (const framework_exception_t &error) {
                  co_return detail::result_access_t::failure<zlink::message_t> (error);
              }
              catch (...) {
                  co_return result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "routed send handler threw an exception");
              }
          }});
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_channel_builder_t &add_request_handler (
      std::string packet_name,
      TReply (TOwner::*method) (const TRequest &, const route_message_context_t &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TRequest> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::request, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TRequest)), std::type_index (typeid (TReply)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &context) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = (owner.*method) (request, context);
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply))));
              }
              catch (const framework_exception_t &error) {
                  return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (...) {
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "routed request handler threw an exception"));
              }
          }});
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_channel_builder_t &add_request_handler (std::string packet_name,
                                                  TReply (TOwner::*method) (const TRequest &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TRequest> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::request, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TRequest)), std::type_index (typeid (TReply)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = (owner.*method) (request);
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply))));
              }
              catch (const framework_exception_t &error) {
                  return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
              }
              catch (...) {
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "routed request handler threw an exception"));
              }
          }});
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_channel_builder_t &
    add_request_handler (std::string packet_name,
                         task_t<TReply> (TOwner::*method) (const TRequest &))
    {
        const auto packet =
          packet_name.empty () ? detail::message_name<TRequest> () : std::move (packet_name);
        return add_handler (route_handler_registration_t{
          route_handler_kind_t::request, packet, std::type_index (typeid (TOwner)),
          std::type_index (typeid (TRequest)), std::type_index (typeid (TReply)),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const route_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = co_await (owner.*method) (request);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              }
              catch (const framework_exception_t &error) {
                  co_return detail::result_access_t::failure<zlink::message_t> (error);
              }
              catch (...) {
                  co_return result_t<zlink::message_t>::failure (
                    framework_error_kind_t::internal_failure,
                    "routed request handler threw an exception");
              }
          }});
    }

  private:
    friend class zlink_builder_t;
    friend void detail::connect_route_channel_peer (route_channel_builder_t &,
                                                   zlink::routing_id_t,
                                                   std::string);
    explicit route_channel_builder_t (std::shared_ptr<detail::route_channel_builder_state_t> state);

    route_channel_builder_t &add_handler (route_handler_registration_t registration);

    std::shared_ptr<detail::route_channel_builder_state_t> _state;
};

class message_bus_t
{
  public:
    using payload_encoder_t = std::function<encoded_payload_t (serializer_registry_t &)>;

    message_bus_t ();
    ~message_bus_t ();

    message_bus_t (message_bus_t &&) noexcept;
    message_bus_t &operator= (message_bus_t &&) noexcept;
    message_bus_t (const message_bus_t &) = default;
    message_bus_t &operator= (const message_bus_t &) = default;

    template <typename TRequest>
    channel_request_call_t request (std::string channel_name, TRequest request)
    {
        auto state = _state;
        auto preflight = _preflight;
        auto request_value = std::make_shared<TRequest> (std::move (request));
        return channel_request_call_t (
          detail::message_name<TRequest> (), serializers (),
          [state, preflight = std::move (preflight),
           channel_name = std::move (channel_name),
           request_value] (const std::string &packet_name, std::chrono::milliseconds timeout,
                           const channel_request_call_t::metadata_map_t &metadata) {
              if (preflight) {
                  const auto admitted = preflight ();
                  if (!admitted) {
                      return task_t<zlink::message_t> (
                        result_t<zlink::message_t>::failure (
                          admitted.error_kind (),
                          admitted.error ()
                            ? admitted.error ()->what ()
                            : "channel request preflight failed"));
                  }
              }
              auto bus = message_bus_t (state);
              const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                               ? timeout
                                               : bus.default_request_timeout (channel_name);
              return bus.submit_request_message_async (
                channel_name, packet_name, std::type_index (typeid (TRequest)),
                [request_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TRequest> ().serialize (*request_value);
                },
                effective_timeout, metadata);
          });
    }

    template <typename TMessage> send_call_t send (std::string channel_name, TMessage message)
    {
        auto state = _state;
        auto preflight = _preflight;
        auto message_value = std::make_shared<TMessage> (std::move (message));
        return send_call_t (
          detail::message_name<TMessage> (),
          [state, preflight = std::move (preflight),
           channel_name = std::move (channel_name),
           message_value] (const std::string &packet_name,
                           const send_call_t::metadata_map_t &metadata) {
              if (preflight) {
                  const auto admitted = preflight ();
                  if (!admitted)
                      return admitted;
              }
              return message_bus_t (state).submit_send (
                channel_name, packet_name, std::type_index (typeid (TMessage)),
                [message_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TMessage> ().serialize (*message_value);
                },
                std::chrono::milliseconds::zero (), metadata);
          });
    }

    template <typename TEvent>
    send_call_t publish (std::string channel_name, std::string topic, TEvent event)
    {
        auto preflight = _preflight;
        auto event_value = std::make_shared<TEvent> (std::move (event));
        return send_call_t (
          detail::message_name<TEvent> (),
          [state = _state, preflight = std::move (preflight),
           channel_name = std::move (channel_name), topic = std::move (topic),
           event_value] (const std::string &packet_name,
                         const send_call_t::metadata_map_t &metadata) {
              if (preflight) {
                  const auto admitted = preflight ();
                  if (!admitted)
                      return admitted;
              }
              return message_bus_t (state).submit_publish (
                channel_name, topic, packet_name, std::type_index (typeid (TEvent)),
                [event_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TEvent> ().serialize (*event_value);
                },
                std::chrono::milliseconds::zero (), metadata);
          });
    }

    std::chrono::milliseconds default_request_timeout (const std::string &channel_name) const;

  private:
    friend class zlink_builder_t;
    friend class request_client_t;
    friend class publisher_t;
    friend class spot_context_t;
    friend class channel_runtime_options_t;
    friend class detail::channel_outbound_exchange_t;
    friend class detail::channel_runtime_t;
    friend class detail::channel_runtime_manager_t;

    class erased_request_result_t
    {
      public:
        explicit erased_request_result_t (framework_exception_t error);
        erased_request_result_t (zlink::message_t reply, serializer_registry_t &serializers);

        result_t<zlink::message_t> message () const
        {
            if (_reply) {
                return result_t<zlink::message_t>::success (*_reply);
            }
            return detail::result_access_t::failure<zlink::message_t> (_error);
        }

        template <typename TReply> result_t<TReply> as () const
        {
            if (_reply) {
                if (_serializers == nullptr) {
                    return result_t<TReply>::failure (
                      framework_error_kind_t::protocol_error,
                      "channel request has no serializer registry");
                }
                try {
                    return result_t<TReply>::success (_serializers->get<TReply> ().deserialize (
                      detail::encoded_payload_from_raw (*_reply)));
                }
                catch (const framework_exception_t &error) {
                    return detail::result_access_t::failure<TReply> (error);
                }
            }
            return detail::result_access_t::failure<TReply> (_error);
        }

      private:
        framework_exception_t _error;
        std::optional<zlink::message_t> _reply;
        serializer_registry_t *_serializers = nullptr;
    };

    explicit message_bus_t (std::shared_ptr<detail::channel_runtime_state_t> state);
    message_bus_t (
      std::shared_ptr<detail::channel_runtime_state_t> state,
      std::function<result_t<void> ()> preflight);

    erased_request_result_t submit_request (std::string channel_name,
                                            std::string packet_name,
                                            std::type_index request_type,
                                            payload_encoder_t encode_payload,
                                            std::chrono::milliseconds timeout,
                                            const channel_request_call_t::metadata_map_t &metadata);
    task_t<zlink::message_t>
    submit_request_message_async (std::string channel_name,
                                  std::string packet_name,
                                  std::type_index request_type,
                                  payload_encoder_t encode_payload,
                                  std::chrono::milliseconds timeout,
                                  channel_request_call_t::metadata_map_t metadata);
    serializer_registry_t *serializers () const noexcept;
    result_t<void> submit_send (std::string channel_name,
                                std::string packet_name,
                                std::type_index message_type,
                                payload_encoder_t encode_payload,
                                std::chrono::milliseconds timeout,
                                const send_call_t::metadata_map_t &metadata);
    result_t<void> submit_publish (std::string channel_name,
                                   std::string topic,
                                   std::string packet_name,
                                   std::type_index event_type,
                                   payload_encoder_t encode_payload,
                                   std::chrono::milliseconds timeout,
                                   const send_call_t::metadata_map_t &metadata);

    std::shared_ptr<detail::channel_runtime_state_t> _state;
    std::function<result_t<void> ()> _preflight;
};

class channel_server_socket_runtime_options_t
{
  public:
    channel_server_socket_runtime_options_t ();
    ~channel_server_socket_runtime_options_t ();

    channel_server_socket_runtime_options_t (channel_server_socket_runtime_options_t &&) noexcept;
    channel_server_socket_runtime_options_t &
    operator= (channel_server_socket_runtime_options_t &&) noexcept;
    channel_server_socket_runtime_options_t (const channel_server_socket_runtime_options_t &) =
      default;
    channel_server_socket_runtime_options_t &
    operator= (const channel_server_socket_runtime_options_t &) = default;

    channel_server_socket_runtime_options_t &peer_weight (zlink::peer_weight_t value);
    channel_server_socket_runtime_options_t &weight (int value);

  private:
    friend class client_server_channel_runtime_options_t;
    friend class route_mesh_channel_runtime_options_t;
    channel_server_socket_runtime_options_t (std::shared_ptr<detail::channel_runtime_state_t> state,
                                             std::string channel_name);

    std::shared_ptr<detail::channel_runtime_state_t> _state;
    std::string _channel_name;
};

class client_server_channel_runtime_options_t
{
  public:
    client_server_channel_runtime_options_t ();
    ~client_server_channel_runtime_options_t ();

    client_server_channel_runtime_options_t (client_server_channel_runtime_options_t &&) noexcept;
    client_server_channel_runtime_options_t &
    operator= (client_server_channel_runtime_options_t &&) noexcept;
    client_server_channel_runtime_options_t (const client_server_channel_runtime_options_t &) =
      default;
    client_server_channel_runtime_options_t &
    operator= (const client_server_channel_runtime_options_t &) = default;

    channel_server_socket_runtime_options_t configure_server_socket () const;

  private:
    friend class channel_runtime_options_t;
    client_server_channel_runtime_options_t (std::shared_ptr<detail::channel_runtime_state_t> state,
                                             std::string channel_name);

    std::shared_ptr<detail::channel_runtime_state_t> _state;
    std::string _channel_name;
};

class route_mesh_channel_runtime_options_t
{
  public:
    route_mesh_channel_runtime_options_t ();
    ~route_mesh_channel_runtime_options_t ();

    route_mesh_channel_runtime_options_t (route_mesh_channel_runtime_options_t &&) noexcept;
    route_mesh_channel_runtime_options_t &
    operator= (route_mesh_channel_runtime_options_t &&) noexcept;
    route_mesh_channel_runtime_options_t (const route_mesh_channel_runtime_options_t &) = default;
    route_mesh_channel_runtime_options_t &
    operator= (const route_mesh_channel_runtime_options_t &) = default;

    channel_server_socket_runtime_options_t configure_server_socket () const;

  private:
    friend class channel_runtime_options_t;
    route_mesh_channel_runtime_options_t (std::shared_ptr<detail::channel_runtime_state_t> state,
                                          std::string channel_name);

    std::shared_ptr<detail::channel_runtime_state_t> _state;
    std::string _channel_name;
};

class channel_runtime_options_t
{
  public:
    channel_runtime_options_t ();
    explicit channel_runtime_options_t (message_bus_t bus);
    ~channel_runtime_options_t ();

    channel_runtime_options_t (channel_runtime_options_t &&) noexcept;
    channel_runtime_options_t &operator= (channel_runtime_options_t &&) noexcept;
    channel_runtime_options_t (const channel_runtime_options_t &) = default;
    channel_runtime_options_t &operator= (const channel_runtime_options_t &) = default;

    client_server_channel_runtime_options_t client_server_channel (std::string channel_name) const;
    route_mesh_channel_runtime_options_t route_mesh_channel (std::string_view channel_name) const;

  private:
    std::shared_ptr<detail::channel_runtime_state_t> _state;
};

class route_send_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<result_t<void> (const std::string &, const metadata_map_t &)>;

    route_send_call_t (std::string packet_name, submit_fn_t submit);

    route_send_call_t &metadata (std::string key, std::string value);
    task_t<void> submit ();

  private:
    result_t<void> submit_now ();

    std::string _packet_name;
    metadata_map_t _metadata;
    submit_fn_t _submit;
    std::shared_ptr<detail::submit_once_t> _submission =
      std::make_shared<detail::submit_once_t> ();
};

namespace detail
{
struct spot_activation_intent_t
{
    bool instance = false;
    std::optional<std::string> stable_type;
    std::optional<std::string> mesh_name;
};
}

class spot_send_call_t : public route_send_call_t
{
  public:
    spot_send_call_t (std::string packet_name,
                      submit_fn_t submit,
                      std::shared_ptr<detail::spot_activation_intent_t> intent) :
        route_send_call_t (std::move (packet_name), std::move (submit)),
        _intent (std::move (intent))
    {
    }

    spot_send_call_t &metadata (std::string key, std::string value)
    {
        route_send_call_t::metadata (std::move (key), std::move (value));
        return *this;
    }
    spot_send_call_t &instance_spot () { return set_instance (std::nullopt); }
    spot_send_call_t &instance_spot (std::string stable_type)
    {
        return set_instance (std::move (stable_type));
    }
    spot_send_call_t &in_mesh (std::string mesh_name)
    {
        if (_intent->mesh_name) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Spot mesh can be selected only once");
        }
        _intent->mesh_name = std::move (mesh_name);
        return *this;
    }

  private:
    spot_send_call_t &set_instance (std::optional<std::string> stable_type)
    {
        if (_intent->instance) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Instance Spot intent can be selected only once");
        }
        _intent->instance = true;
        _intent->stable_type = std::move (stable_type);
        return *this;
    }
    std::shared_ptr<detail::spot_activation_intent_t> _intent;
};

class spot_request_call_t : public channel_request_call_t
{
  public:
    spot_request_call_t (std::string packet_name,
                         serializer_registry_t *serializers,
                         submit_fn_t submit,
                         std::shared_ptr<detail::spot_activation_intent_t> intent) :
        channel_request_call_t (std::move (packet_name), serializers, std::move (submit)),
        _intent (std::move (intent))
    {
    }

    spot_request_call_t &timeout (std::chrono::milliseconds value)
    {
        channel_request_call_t::timeout (value);
        return *this;
    }
    spot_request_call_t &metadata (std::string key, std::string value)
    {
        channel_request_call_t::metadata (std::move (key), std::move (value));
        return *this;
    }
    spot_request_call_t &instance_spot () { return set_instance (std::nullopt); }
    spot_request_call_t &instance_spot (std::string stable_type)
    {
        return set_instance (std::move (stable_type));
    }
    spot_request_call_t &in_mesh (std::string mesh_name)
    {
        if (_intent->mesh_name) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Spot mesh can be selected only once");
        }
        _intent->mesh_name = std::move (mesh_name);
        return *this;
    }

  private:
    spot_request_call_t &set_instance (std::optional<std::string> stable_type)
    {
        if (_intent->instance) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Instance Spot intent can be selected only once");
        }
        _intent->instance = true;
        _intent->stable_type = std::move (stable_type);
        return *this;
    }
    std::shared_ptr<detail::spot_activation_intent_t> _intent;
};

class route_client_t
{
  public:
    using payload_encoder_t = std::function<encoded_payload_t (serializer_registry_t &)>;

    route_client_t ();
    ~route_client_t ();

    route_client_t (route_client_t &&) noexcept;
    route_client_t &operator= (route_client_t &&) noexcept;
    route_client_t (const route_client_t &) = default;
    route_client_t &operator= (const route_client_t &) = default;

    template <typename TMessage>
    route_send_call_t
    send_to_node (std::string router_channel_id, zlink::routing_id_t target_node_rid, TMessage message)
    {
        auto state = _state;
        auto message_value = std::make_shared<TMessage> (std::move (message));
        return route_send_call_t (
          detail::message_name<TMessage> (),
          [state, router_channel_id = std::move (router_channel_id),
           target_node_rid = std::move (target_node_rid),
           message_value] (const std::string &packet_name,
                           const route_send_call_t::metadata_map_t &metadata) -> result_t<void> {
              return submit_send_erased (
                state, router_channel_id, target_node_rid, packet_name,
                std::type_index (typeid (TMessage)),
                [message_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TMessage> ().serialize (*message_value);
                },
                metadata);
          });
    }

    template <typename TMessage>
    route_send_call_t send_to_channel (std::string channel_name, TMessage message)
    {
        auto state = _state;
        auto message_value = std::make_shared<TMessage> (std::move (message));
        return route_send_call_t (
          detail::message_name<TMessage> (),
          [state, channel_name = std::move (channel_name),
           message_value] (const std::string &packet_name,
                           const route_send_call_t::metadata_map_t &metadata) -> result_t<void> {
              return submit_channel_send_erased (
                state, channel_name, packet_name, std::type_index (typeid (TMessage)),
                [message_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TMessage> ().serialize (*message_value);
                },
                metadata);
          });
    }

    template <typename TMessage>
    spot_send_call_t send_to_spot (spot_id_t target, TMessage message)
    {
        auto state = _state;
        auto message_value = std::make_shared<TMessage> (std::move (message));
        auto intent = std::make_shared<detail::spot_activation_intent_t> ();
        return spot_send_call_t (
          detail::message_name<TMessage> (),
          [state, target = std::move (target), intent,
           message_value] (const std::string &packet_name,
                           const route_send_call_t::metadata_map_t &metadata) -> result_t<void> {
              return submit_spot_id_send_erased (
                state, target, *intent, packet_name, std::type_index (typeid (TMessage)),
                [message_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TMessage> ().serialize (*message_value);
                },
                metadata);
          }, intent);
    }

    template <typename TRequest>
    channel_request_call_t
    request_to_node (std::string router_channel_id, zlink::routing_id_t target_node_rid, TRequest request)
    {
        auto state = _state;
        auto request_value = std::make_shared<TRequest> (std::move (request));
        return channel_request_call_t (
          detail::message_name<TRequest> (), _serializers,
          [state, router_channel_id = std::move (router_channel_id),
           target_node_rid = std::move (target_node_rid), request_value] (
            const std::string &packet_name, std::chrono::milliseconds timeout,
            const channel_request_call_t::metadata_map_t &metadata) -> task_t<zlink::message_t> {
              return submit_request_reply_message_erased (
                state, router_channel_id, target_node_rid, packet_name,
                std::type_index (typeid (TRequest)),
                [request_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TRequest> ().serialize (*request_value);
                },
                timeout, metadata);
          });
    }

    template <typename TRequest>
    channel_request_call_t request_to_channel (std::string channel_name, TRequest request)
    {
        auto state = _state;
        auto request_value = std::make_shared<TRequest> (std::move (request));
        return channel_request_call_t (
          detail::message_name<TRequest> (), _serializers,
          [state, channel_name = std::move (channel_name), request_value] (
            const std::string &packet_name, std::chrono::milliseconds timeout,
            const channel_request_call_t::metadata_map_t &metadata) -> task_t<zlink::message_t> {
              return submit_channel_request_reply_message_erased (
                state, channel_name, packet_name, std::type_index (typeid (TRequest)),
                [request_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TRequest> ().serialize (*request_value);
                },
                timeout, metadata);
          });
    }

    /* Direct Spot requests resolve the global SpotId once for this operation. */
    template <typename TRequest>
    spot_request_call_t request_to_spot (spot_id_t target, TRequest request)
    {
        auto state = _state;
        auto request_value = std::make_shared<TRequest> (std::move (request));
        auto intent = std::make_shared<detail::spot_activation_intent_t> ();
        return spot_request_call_t (
          detail::message_name<TRequest> (), _serializers,
          [state, target = std::move (target), intent,
           request_value] (const std::string &packet_name, std::chrono::milliseconds timeout,
                           const channel_request_call_t::metadata_map_t &metadata) mutable
          -> task_t<zlink::message_t> {
              return submit_spot_id_request_reply_message_erased (
                state, target, *intent, packet_name, std::type_index (typeid (TRequest)),
                [request_value] (serializer_registry_t &serializers) {
                    return serializers.template get<TRequest> ().serialize (*request_value);
                },
                timeout, metadata);
          }, intent);
    }

  private:
    friend class zlink_builder_t;
    friend class detail::route_client_runtime_t;
    explicit route_client_t (std::shared_ptr<detail::route_client_state_t> state,
                             serializer_registry_t &serializers);

    static result_t<void>
    submit_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                        const std::string &router_channel_id,
                        const zlink::routing_id_t &target_node_rid,
                        const std::string &packet_name,
                        std::type_index message_type,
                        payload_encoder_t encode_payload,
                        const route_send_call_t::metadata_map_t &metadata);

    static result_t<void>
    submit_channel_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                const std::string &channel_name,
                                const std::string &packet_name,
                                std::type_index message_type,
                                payload_encoder_t encode_payload,
                                const route_send_call_t::metadata_map_t &metadata);

    static task_t<std::uint64_t>
    submit_request_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                           const std::string &router_channel_id,
                           const zlink::routing_id_t &target_node_rid,
                           const std::string &packet_name,
                           std::type_index request_type,
                           payload_encoder_t encode_payload,
                           std::chrono::milliseconds timeout,
                           const channel_request_call_t::metadata_map_t &metadata);

    static result_t<void>
    submit_spot_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                             const std::string &router_channel_id,
                             const zlink::routing_id_t &target_node_rid,
                             const spot_id_t &spot_target,
                             std::uint64_t target_spot_generation,
                             const std::string &packet_name,
                             std::type_index message_type,
                             payload_encoder_t encode_payload,
                             const route_send_call_t::metadata_map_t &metadata);

    static task_t<std::uint64_t>
    submit_spot_request_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                const std::string &router_channel_id,
                                const zlink::routing_id_t &target_node_rid,
                                const spot_id_t &spot_target,
                                const std::string &packet_name,
                                std::type_index request_type,
                                payload_encoder_t encode_payload,
                                std::chrono::milliseconds timeout,
                                const channel_request_call_t::metadata_map_t &metadata);

    template <typename TReply>
    static task_t<TReply>
    submit_request_reply_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                 serializer_registry_t *serializers,
                                 std::string router_channel_id,
                                 zlink::routing_id_t target_node_rid,
                                 std::string packet_name,
                                 std::type_index request_type,
                                 payload_encoder_t encode_payload,
                                 std::chrono::milliseconds timeout,
                                 const std::map<std::string, std::string> &metadata);

    static task_t<zlink::message_t>
    submit_request_reply_message_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                         std::string router_channel_id,
                                         zlink::routing_id_t target_node_rid,
                                         std::string packet_name,
                                         std::type_index request_type,
                                         payload_encoder_t encode_payload,
                                         std::chrono::milliseconds timeout,
                                         std::map<std::string, std::string> metadata);

    static task_t<zlink::message_t> submit_channel_request_reply_message_erased (
      const std::shared_ptr<detail::route_client_state_t> &state,
      std::string channel_name,
      std::string packet_name,
      std::type_index request_type,
      payload_encoder_t encode_payload,
      std::chrono::milliseconds timeout,
      std::map<std::string, std::string> metadata);

    static task_t<zlink::message_t> submit_spot_request_reply_message_erased (
      const std::shared_ptr<detail::route_client_state_t> &state,
      std::string router_channel_id,
      zlink::routing_id_t target_node_rid,
      spot_id_t spot_target,
      std::uint64_t target_spot_generation,
      std::string packet_name,
      std::type_index request_type,
      payload_encoder_t encode_payload,
      std::chrono::milliseconds timeout,
      std::map<std::string, std::string> metadata);

    static result_t<void>
    submit_spot_id_send_erased (const std::shared_ptr<detail::route_client_state_t> &state,
                                    const spot_id_t &target,
                                    const detail::spot_activation_intent_t &intent,
                                    const std::string &packet_name,
                                    std::type_index message_type,
                                    payload_encoder_t encode_payload,
                                    const route_send_call_t::metadata_map_t &metadata);

    static task_t<zlink::message_t> submit_spot_id_request_reply_message_erased (
      const std::shared_ptr<detail::route_client_state_t> &state,
      spot_id_t target,
      detail::spot_activation_intent_t intent,
      std::string packet_name,
      std::type_index request_type,
      payload_encoder_t encode_payload,
      std::chrono::milliseconds timeout,
      std::map<std::string, std::string> metadata);

    std::shared_ptr<detail::route_client_state_t> _state;
    serializer_registry_t *_serializers = nullptr;
};

template <typename TReply>
task_t<TReply> route_client_t::submit_request_reply_erased (
  const std::shared_ptr<detail::route_client_state_t> &state,
  serializer_registry_t *serializers,
  std::string router_channel_id,
  zlink::routing_id_t target_node_rid,
  std::string packet_name,
  std::type_index request_type,
  payload_encoder_t encode_payload,
  std::chrono::milliseconds timeout,
  const std::map<std::string, std::string> &metadata)
{
    if (serializers == nullptr) {
        co_return result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                             "route client has no serializer registry");
    }
    auto reply = co_await submit_request_reply_message_erased (
      state, std::move (router_channel_id), std::move (target_node_rid), std::move (packet_name),
      request_type, std::move (encode_payload), timeout, metadata);
    try {
        co_return result_t<TReply>::success (
          serializers->get<TReply> ().deserialize (detail::encoded_payload_from_raw (reply)));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<TReply> (error);
    }
}

class request_client_t
{
  public:
    request_client_t (message_bus_t bus, std::string channel_name);

    template <typename TRequest> channel_request_call_t request (TRequest request)
    {
        return _bus.request (_channel_name, std::move (request));
    }

  private:
    message_bus_t _bus;
    std::string _channel_name;
};

class channel_client_t
{
  public:
    explicit channel_client_t (message_bus_t bus) : _bus (std::move (bus)) {}

    template <typename TRequest>
    channel_request_call_t request_to_channel (std::string channel_name, TRequest request)
    {
        return _bus.request (std::move (channel_name), std::move (request));
    }

    template <typename TRequest>
    channel_request_call_t request (std::string channel_name, TRequest request)
    {
        return request_to_channel (std::move (channel_name), std::move (request));
    }

    template <typename TMessage>
    send_call_t send_to_channel (std::string channel_name, TMessage message)
    {
        return _bus.send (std::move (channel_name), std::move (message));
    }

    template <typename TMessage> send_call_t send (std::string channel_name, TMessage message)
    {
        return send_to_channel (std::move (channel_name), std::move (message));
    }

  private:
    message_bus_t _bus;
};

class publisher_t
{
  public:
    explicit publisher_t (message_bus_t bus);

    template <typename TEvent>
    fanout_publish_call_t publish (std::string channel_name, std::string topic, TEvent event)
    {
        return fanout_publish_call_t (
          _bus.publish (std::move (channel_name), std::move (topic), std::move (event)));
    }

  private:
    message_bus_t _bus;
};

} // namespace zlink::framework
