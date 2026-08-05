/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/configuration/detail/framework_options_validation.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/http/http.hpp>
#include <zlink/framework/contracts/locations/stores.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework
{

class client_server_channel_server_builder_t;

class inbound_dispatch_options_t
{
  public:
    explicit inbound_dispatch_options_t (
      std::shared_ptr<detail::framework_options_state_t> options) :
        _options (std::move (options))
    {
    }

    inbound_dispatch_options_t &
    set_application_hwm_bytes (std::optional<std::uint64_t> value)
    {
        _options->application_hwm_bytes = value;
        return *this;
    }

    inbound_dispatch_options_t &
    set_application_hwm_profile (application_hwm_profile_t value)
    {
        _options->application_hwm_profile = value;
        return *this;
    }

    inbound_dispatch_options_t &
    set_process_memory_limit_bytes (std::optional<std::uint64_t> value)
    {
        if (value && *value == 0) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "process memory limit must be positive when specified");
        }
        _options->process_memory_limit_bytes = value;
        return *this;
    }

  private:
    std::shared_ptr<detail::framework_options_state_t> _options;
};

class handler_options_builder_t
{
  public:
    class group_builder_t
    {
      public:
        group_builder_t (handler_options_builder_t &owner, std::string group_name) :
            _owner (&owner), _group_name (std::move (group_name))
        {
            detail::require_non_blank (_group_name, "handler group name is required");
        }

        template <typename THandler> group_builder_t &add ()
        {
            _owner->template add_to_group<THandler> (_group_name);
            return *this;
        }

        template <typename THandler> group_builder_t &add_send ()
        {
            _owner->template add_send_to_group<THandler> (_group_name);
            return *this;
        }

        template <typename THandler> group_builder_t &add_publish ()
        {
            _owner->template add_publish_to_group<THandler> (_group_name);
            return *this;
        }

      private:
        handler_options_builder_t *_owner;
        std::string _group_name;
    };

    handler_options_builder_t (service_collection_t &services,
                               handler_registry_t &handlers,
                               serializer_registry_t &serializers,
                               std::shared_ptr<detail::handler_group_options_state_t> state) :
        _services (&services),
        _handlers (&handlers),
        _serializers (&serializers),
        _state (std::move (state))
    {
    }

    group_builder_t group (std::string group_name)
    {
        return group_builder_t (*this, std::move (group_name));
    }

  private:
    friend class client_server_channel_server_builder_t;

    template <typename THandler>
    void add_to_group (
      std::string group_name,
      std::optional<std::string> packet_name = std::nullopt)
    {
        using request_type = typename THandler::request_type;
        using reply_type = typename THandler::reply_type;
        auto topic_name =
          packet_name && !packet_name->empty ()
            ? std::move (*packet_name)
            : detail::handler_topic_name<THandler, request_type> ();
        _state->add_handler_packet (group_name, detail::handler_group_kind_t::request, topic_name,
                                    detail::message_name<request_type> ());

        detail::injected_handler_registrar_t<
          THandler, typename detail::handler_dependencies_t<THandler>::type>::add (*_services);

        auto *handlers = _handlers;
        add_serializers<request_type> ();
        add_serializers<reply_type> ();
        auto route_group_name = group_name;
        _state->add_installer (std::move (group_name), detail::handler_group_kind_t::request,
                               [handlers, topic_name] (const std::string &channel_name) {
                                   handlers->on_request<THandler, request_type, reply_type> (
                                     channel_name, topic_name, &THandler::handle,
                                     {.execution = handler_execution_t::offload});
                               });
        if constexpr (requires {
                          static_cast<reply_type (THandler::*) (const request_type &)> (
                            &THandler::handle);
                      }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::request,
              [] (route_channel_builder_t &channel) {
                  channel.add_request_handler<THandler, request_type, reply_type> (
                    detail::message_name<request_type> (),
                    static_cast<reply_type (THandler::*) (const request_type &)> (
                      &THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<task_t<reply_type> (THandler::*) (
                                   const request_type &)> (&THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::request,
              [] (route_channel_builder_t &channel) {
                  channel.add_request_handler<THandler, request_type, reply_type> (
                    detail::message_name<request_type> (),
                    static_cast<task_t<reply_type> (THandler::*) (const request_type &)> (
                      &THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<reply_type (THandler::*) (
                                   const request_type &, const route_message_context_t &)> (
                                   &THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::request,
              [] (route_channel_builder_t &channel) {
                  channel.add_request_handler<THandler, request_type, reply_type> (
                    detail::message_name<request_type> (),
                    static_cast<reply_type (THandler::*) (
                      const request_type &, const route_message_context_t &)> (&THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<task_t<reply_type> (THandler::*) (
                                   const request_type &, const route_message_context_t &)> (
                                   &THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::request,
              [] (route_channel_builder_t &channel) {
                  channel.add_request_handler<THandler, request_type, reply_type> (
                    detail::message_name<request_type> (),
                    static_cast<task_t<reply_type> (THandler::*) (
                      const request_type &, const route_message_context_t &)> (&THandler::handle));
              });
        }
    }

    template <typename THandler>
    void add_send_to_group (
      std::string group_name,
      std::optional<std::string> packet_name = std::nullopt)
    {
        using message_type = typename THandler::message_type;
        auto topic_name =
          packet_name && !packet_name->empty ()
            ? std::move (*packet_name)
            : detail::handler_topic_name<THandler, message_type> ();
        _state->add_handler_packet (group_name, detail::handler_group_kind_t::send, topic_name,
                                    detail::message_name<message_type> ());

        detail::injected_handler_registrar_t<
          THandler, typename detail::handler_dependencies_t<THandler>::type>::add (*_services);

        auto *handlers = _handlers;
        add_serializers<message_type> ();
        auto route_group_name = group_name;
        _state->add_installer (std::move (group_name), detail::handler_group_kind_t::send,
                               [handlers, topic_name] (const std::string &channel_name) {
                                   handlers->on_send<THandler, message_type> (
                                     channel_name, topic_name, &THandler::handle,
                                     {.execution = handler_execution_t::offload});
                               });
        if constexpr (requires {
                          static_cast<void (THandler::*) (const message_type &)> (
                            &THandler::handle);
                      }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::send,
              [] (route_channel_builder_t &channel) {
                  channel.add_send_handler<THandler, message_type> (
                    detail::message_name<message_type> (),
                    static_cast<void (THandler::*) (const message_type &)> (&THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<task_t<void> (THandler::*) (const message_type &)> (
                                   &THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::send,
              [] (route_channel_builder_t &channel) {
                  channel.add_send_handler<THandler, message_type> (
                    detail::message_name<message_type> (),
                    static_cast<task_t<void> (THandler::*) (const message_type &)> (
                      &THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<void (THandler::*) (const message_type &,
                                                                 const route_message_context_t &)> (
                                   &THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::send,
              [] (route_channel_builder_t &channel) {
                  channel.add_send_handler<THandler, message_type> (
                    detail::message_name<message_type> (),
                    static_cast<void (THandler::*) (
                      const message_type &, const route_message_context_t &)> (&THandler::handle));
              });
        } else if constexpr (requires {
                                 static_cast<task_t<void> (THandler::*) (
                                   const message_type &, const route_message_context_t &)> (
                                   &THandler::handle);
                             }) {
            _state->add_route_installer (
              std::move (route_group_name), detail::handler_group_kind_t::send,
              [] (route_channel_builder_t &channel) {
                  channel.add_send_handler<THandler, message_type> (
                    detail::message_name<message_type> (),
                    static_cast<task_t<void> (THandler::*) (
                      const message_type &, const route_message_context_t &)> (&THandler::handle));
              });
        }
    }

    template <typename THandler> void add_publish_to_group (std::string group_name)
    {
        using event_type = typename THandler::event_type;
        auto topic_name = detail::handler_topic_name<THandler, event_type> ();
        _state->add_handler_packet (group_name, detail::handler_group_kind_t::publish, topic_name,
                                    detail::message_name<event_type> ());

        detail::injected_handler_registrar_t<
          THandler, typename detail::handler_dependencies_t<THandler>::type>::add (*_services);

        auto *handlers = _handlers;
        add_serializers<event_type> ();
        _state->add_installer (std::move (group_name), detail::handler_group_kind_t::publish,
                               [handlers, topic_name] (const std::string &channel_name) {
                                   handlers->on_event<THandler, event_type> (
                                     channel_name, topic_name, &THandler::handle,
                                     {.execution = handler_execution_t::offload});
                               });
    }

    template <typename TPayload> void add_serializers () {}

    service_collection_t *_services;
    handler_registry_t *_handlers;
    serializer_registry_t *_serializers;
    std::shared_ptr<detail::handler_group_options_state_t> _state;
};

class metadata_policy_builder_t
{
  public:
    explicit metadata_policy_builder_t (
      std::shared_ptr<detail::framework_options_state_t> options) :
        _options (std::move (options))
    {
    }

    metadata_policy_builder_t &add_forwarded_metadata_key (std::string key)
    {
        if (key.empty ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "metadata key must not be empty");
        }
        _options->metadata_policy.add_forwarded_metadata_key (std::move (key));
        return *this;
    }

  private:
    std::shared_ptr<detail::framework_options_state_t> _options;
};

class codec_registration_context_t
{
  public:
    explicit codec_registration_context_t (serializer_registry_t &serializers) :
        _serializers (&serializers)
    {
    }

    template <typename TPayload>
    codec_registration_context_t &
    add_serializer (typename serializer_t<TPayload>::serialize_fn_t serialize,
                    typename serializer_t<TPayload>::deserialize_fn_t deserialize,
                    std::string content_type = "application/octet-stream")
    {
        _serializers->template add<TPayload> (std::move (serialize), std::move (deserialize),
                                              std::move (content_type));
        return *this;
    }

  private:
    serializer_registry_t *_serializers;
};

class codec_options_builder_t
{
  public:
    explicit codec_options_builder_t (serializer_registry_t &serializers) :
        _serializers (&serializers)
    {
    }

    /// Adds a serializer extension. This is for payloads that cannot use the default JSON
    /// serializer, not for listing every application message type.
    template <typename TExtension> codec_options_builder_t &use (const TExtension &extension)
    {
        codec_registration_context_t context (*_serializers);
        extension.register_framework_codecs (context);
        return *this;
    }

  private:
    serializer_registry_t *_serializers;
};

class client_server_channel_client_builder_t;
class client_server_channel_server_builder_t;

class client_server_channel_builder_t
{
  public:
    client_server_channel_builder_t (
      std::string channel_name,
      std::shared_ptr<detail::framework_options_state_t> options,
      std::shared_ptr<detail::handler_group_options_state_t> handler_groups,
      service_collection_t &services,
      handler_registry_t &handlers,
      serializer_registry_t &serializers) :
        _channel_name (std::move (channel_name)),
        _options (std::move (options)),
        _handler_groups (std::move (handler_groups)),
        _services (&services),
        _handlers (&handlers),
        _serializers (&serializers)
    {
        detail::require_non_blank (_channel_name, "client/server channel name is required");
        _options->client_server_channels.insert (_channel_name);
    }

    client_server_channel_client_builder_t client ();
    client_server_channel_server_builder_t server ();

  private:
    friend class client_server_channel_client_builder_t;
    friend class client_server_channel_server_builder_t;

    void select_server ()
    {
        ++_options->client_server_server_registration_counts[_channel_name];
        _options->client_server_channels_with_server.insert (_channel_name);
    }

    void listen (std::uint16_t port)
    {
        _server_port = port;
        _server_endpoint =
          "tcp://" + _server_bind_host + ":"
          + (port == 0 ? "*" : std::to_string (port));
        apply_channel ();
    }

    void set_server_bind_host (std::string host)
    {
        detail::require_non_blank (
          host, "client/server server bind host is required");
        _server_bind_host = std::move (host);
        if (_server_port)
            listen (*_server_port);
    }

    void set_server_advertise_host (std::string host)
    {
        detail::require_non_blank (
          host, "client/server server advertise host is required");
        _server_advertise_host = std::move (host);
        _options->client_server_server_advertise_hosts[
          _channel_name] = *_server_advertise_host;
    }

    void set_server_weight (int weight)
    {
        if (weight < 0 || weight > 10000)
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "client/server server weight must be between 0 and 10000");
        _server_weight = weight;
        apply_channel ();
    }

    void select_client ()
    {
        ++_options->client_server_client_registration_counts[_channel_name];
        _options->client_server_channels_with_client.insert (_channel_name);
        _client_enabled = true;
        apply_channel ();
    }

    void connect_client (std::string endpoint)
    {
        detail::require_non_blank (
          endpoint, "client/server client endpoint is required");
        client_connections ().connect (std::move (endpoint));
        apply_channel ();
    }

    endpoint_connections_t client_connections ()
    {
        return _options->client_endpoint_connections[_channel_name];
    }

    void add_server_handler_group (std::string group_name)
    {
        detail::require_non_blank (group_name, "handler group name is required");
        _handler_groups->add_channel (
          std::move (group_name), _channel_name,
          {detail::handler_group_kind_t::request, detail::handler_group_kind_t::send},
          "client/server channel");
    }

    void apply_channel ()
    {
        const auto channel_name = _channel_name;
        const auto server_endpoint = _server_endpoint;
        const auto server_weight = _server_weight;
        const auto client_enabled = _client_enabled;
        const auto client_endpoints =
          _options->client_endpoint_connections[_channel_name].list_connections ();
        if (!server_endpoint.empty ()) {
            _options->client_server_server_actions[channel_name] =
              [server_endpoint,
               server_weight] (channel_builder_t &channel) {
                  auto server = channel.enable_server ();
                  server.service_weight (server_weight);
                  server.bind (server_endpoint);
              };
        }
        if (client_enabled) {
            _options->client_server_client_actions[channel_name] =
              [client_endpoints] (
                channel_builder_t &channel) {
                  auto client = channel.enable_client ();
                  for (const auto &endpoint : client_endpoints) {
                      client.connect (endpoint);
                  }
              };
        }
        const auto merged_timeout =
          _options->client_server_default_request_timeouts[channel_name];
        const auto merged_server =
          _options->client_server_server_actions.contains (channel_name)
            ? _options->client_server_server_actions.at (channel_name)
            : std::function<void (channel_builder_t &)>{};
        const auto merged_client =
          _options->client_server_client_actions.contains (channel_name)
            ? _options->client_server_client_actions.at (channel_name)
            : std::function<void (channel_builder_t &)>{};
        _options->set_zlink_action (
          "client_server_channel:" + channel_name,
          [channel_name, merged_timeout, merged_server,
           merged_client] (zlink_builder_t &zlink) {
              auto channel = zlink.channel (channel_name);
              if (merged_timeout) {
                  channel.default_request_timeout (*merged_timeout);
              }
              if (merged_server) {
                  merged_server (channel);
              }
              if (merged_client) {
                  merged_client (channel);
              }
          });
    }

    std::string _channel_name;
    std::shared_ptr<detail::framework_options_state_t> _options;
    std::shared_ptr<detail::handler_group_options_state_t> _handler_groups;
    service_collection_t *_services;
    handler_registry_t *_handlers;
    serializer_registry_t *_serializers;
    std::string _server_endpoint;
    std::string _server_bind_host = "0.0.0.0";
    std::optional<std::string> _server_advertise_host;
    std::optional<std::uint16_t> _server_port;
    int _server_weight = 100;
    bool _client_enabled = false;
    std::size_t _inline_handler_sequence = 0;
};

class client_server_channel_client_builder_t
{
  public:
    client_server_channel_client_builder_t &
    connect (std::string endpoint)
    {
        _channel->connect_client (std::move (endpoint));
        return *this;
    }

  private:
    friend class client_server_channel_builder_t;
    explicit client_server_channel_client_builder_t (
      std::shared_ptr<client_server_channel_builder_t> channel) :
        _channel (std::move (channel))
    {
    }

    std::shared_ptr<client_server_channel_builder_t> _channel;
};

class client_server_channel_server_builder_t
{
  public:
    client_server_channel_server_builder_t &
    listen (std::uint16_t port = 0)
    {
        _channel->listen (port);
        return *this;
    }

    client_server_channel_server_builder_t &
    set_bind_host (std::string host)
    {
        _channel->set_server_bind_host (std::move (host));
        return *this;
    }

    client_server_channel_server_builder_t &
    set_advertise_host (std::string host)
    {
        _channel->set_server_advertise_host (
          std::move (host));
        return *this;
    }

    client_server_channel_server_builder_t &
    set_weight (int weight)
    {
        _channel->set_server_weight (weight);
        return *this;
    }

    client_server_channel_server_builder_t &
    add_handler_group (std::string group_name)
    {
        _channel->add_server_handler_group (
          std::move (group_name));
        return *this;
    }

    template <typename THandler, typename TMessage>
    client_server_channel_server_builder_t &
    add_send_handler (std::string packet_name = {})
    {
        static_assert (
          std::is_same_v<
            typename THandler::message_type, TMessage>,
          "ClientServer send handler message type must match THandler::message_type");
        const auto group_name = next_inline_group ("send");
        handler_options_builder_t handlers (
          *_channel->_services, *_channel->_handlers,
          *_channel->_serializers, _channel->_handler_groups);
        handlers.template add_send_to_group<THandler> (
          group_name, std::move (packet_name));
        _channel->add_server_handler_group (group_name);
        return *this;
    }

    template <typename THandler, typename TRequest, typename TReply>
    client_server_channel_server_builder_t &
    add_request_handler (std::string packet_name = {})
    {
        static_assert (
          std::is_same_v<
              typename THandler::request_type, TRequest>
            && std::is_same_v<
              typename THandler::reply_type, TReply>,
          "ClientServer request/reply types must match THandler");
        const auto group_name = next_inline_group ("request");
        handler_options_builder_t handlers (
          *_channel->_services, *_channel->_handlers,
          *_channel->_serializers, _channel->_handler_groups);
        handlers.template add_to_group<THandler> (
          group_name, std::move (packet_name));
        _channel->add_server_handler_group (group_name);
        return *this;
    }

  private:
    friend class client_server_channel_builder_t;
    explicit client_server_channel_server_builder_t (
      std::shared_ptr<client_server_channel_builder_t> channel) :
        _channel (std::move (channel))
    {
    }

    std::string next_inline_group (std::string_view kind)
    {
        return "__client_server:" + _channel->_channel_name
               + ":" + std::string (kind) + ":"
               + std::to_string (
                 _channel->_inline_handler_sequence++);
    }

    std::shared_ptr<client_server_channel_builder_t> _channel;
};

inline client_server_channel_client_builder_t
client_server_channel_builder_t::client ()
{
    auto channel =
      std::make_shared<client_server_channel_builder_t> (*this);
    channel->select_client ();
    return client_server_channel_client_builder_t (
      std::move (channel));
}

inline client_server_channel_server_builder_t
client_server_channel_builder_t::server ()
{
    auto channel =
      std::make_shared<client_server_channel_builder_t> (*this);
    channel->select_server ();
    return client_server_channel_server_builder_t (
      std::move (channel));
}

class fanout_channel_builder_t
{
  public:
    fanout_channel_builder_t (
      std::string channel_name,
      std::shared_ptr<detail::framework_options_state_t> options,
      std::shared_ptr<detail::handler_group_options_state_t> handler_groups) :
        _channel_name (std::move (channel_name)),
        _options (std::move (options)),
        _handler_groups (std::move (handler_groups))
    {
        detail::require_non_blank (_channel_name, "fanout channel name is required");
        _options->fanout_channels.insert (_channel_name);
    }

    fanout_channel_builder_t &enable_publisher (std::string endpoint)
    {
        detail::require_non_blank (endpoint, "fanout publisher endpoint is required");
        _publisher_endpoint = std::move (endpoint);
        apply ();
        return *this;
    }

    fanout_channel_builder_t &set_routing_id (zlink::routing_id_t routing_id)
    {
        _routing_id = std::move (routing_id);
        apply ();
        return *this;
    }

    fanout_channel_builder_t &enable_subscriber ()
    {
        _subscriber_enabled = true;
        _options->fanout_channels_with_automatic_subscriber.insert (_channel_name);
        apply ();
        return *this;
    }

    fanout_channel_builder_t &connect (std::string endpoint)
    {
        detail::require_non_blank (endpoint, "fanout subscriber endpoint is required");
        _subscriber_enabled = true;
        subscriber_connections ().connect (std::move (endpoint));
        _options->fanout_channels_with_manual_subscriber.insert (_channel_name);
        apply ();
        return *this;
    }

    endpoint_connections_t subscriber_connections ()
    {
        return _options->subscriber_endpoint_connections[_channel_name];
    }

    fanout_channel_builder_t &use_handler_group (std::string group_name)
    {
        detail::require_non_blank (group_name, "handler group name is required");
        _handler_groups->add_channel (std::move (group_name), _channel_name,
                                      {detail::handler_group_kind_t::publish}, "fanout channel");
        return *this;
    }

  private:
    void apply ()
    {
        const auto channel_name = _channel_name;
        const auto publisher_endpoint = _publisher_endpoint;
        const auto routing_id = _routing_id;
        const auto subscriber_enabled = _subscriber_enabled;
        const auto subscriber_endpoints =
          _options->subscriber_endpoint_connections[_channel_name].list_connections ();
        const auto subscriber_uses_discovery =
          _options->fanout_channels_with_automatic_subscriber.contains (channel_name);
        if (subscriber_enabled) {
            _options->fanout_channels_with_subscriber.insert (channel_name);
        } else {
            _options->fanout_channels_with_subscriber.erase (channel_name);
        }
        if (!publisher_endpoint.empty ()) {
            _options->fanout_channels_with_publisher.insert (channel_name);
        } else {
            _options->fanout_channels_with_publisher.erase (channel_name);
        }
        _options->set_zlink_action ("fanout_channel:" + channel_name,
                                    [channel_name, publisher_endpoint, subscriber_enabled,
                                     subscriber_endpoints, routing_id,
                                     subscriber_uses_discovery] (zlink_builder_t &zlink) {
                                        auto channel = zlink.channel (channel_name);
                                        if (!publisher_endpoint.empty ()) {
                                            auto publisher = channel.enable_publisher ();
                                            if (routing_id) {
                                                publisher.set_routing_id (*routing_id);
                                            }
                                            publisher.bind (publisher_endpoint);
                                        }
                                        if (subscriber_enabled) {
                                            auto subscriber = channel.enable_subscriber ();
                                            if (!subscriber_uses_discovery) {
                                                for (const auto &endpoint : subscriber_endpoints) {
                                                    subscriber.connect (endpoint);
                                                }
                                            }
                                        }
                                    });
    }

    std::string _channel_name;
    std::shared_ptr<detail::framework_options_state_t> _options;
    std::shared_ptr<detail::handler_group_options_state_t> _handler_groups;
    std::string _publisher_endpoint;
    std::optional<zlink::routing_id_t> _routing_id;
    bool _subscriber_enabled = false;
};

class route_mesh_channel_builder_t
{
  public:
    route_mesh_channel_builder_t (
      std::string channel_name,
      std::shared_ptr<detail::framework_options_state_t> options,
      std::shared_ptr<detail::handler_group_options_state_t> handler_groups) :
        _channel_name (std::move (channel_name)),
        _options (std::move (options)),
        _handler_groups (std::move (handler_groups))
    {
        detail::require_non_blank (_channel_name, "route mesh channel name is required");
        _options->route_mesh_channels.insert (_channel_name);
        apply ();
    }

    route_mesh_channel_builder_t &enable_server (std::string endpoint)
    {
        detail::require_non_blank (endpoint, "route mesh server endpoint is required");
        _bind_endpoint = std::move (endpoint);
        _options->route_mesh_channels_with_bind.insert (_channel_name);
        apply ();
        return *this;
    }

    route_mesh_channel_builder_t &set_routing_id (zlink::routing_id_t routing_id)
    {
        _routing_id = std::move (routing_id);
        apply ();
        return *this;
    }

    route_mesh_channel_builder_t &enable_client ()
    {
        _options->route_mesh_channels_with_client.insert (_channel_name);
        _manual_connections.clear ();
        apply ();
        return *this;
    }

    route_mesh_channel_builder_t &enable_client (std::string endpoint)
    {
        detail::require_non_blank (endpoint, "route mesh client endpoint is required");
        _options->route_mesh_channels_with_client.insert (_channel_name);
        _manual_connections.push_back (std::move (endpoint));
        apply ();
        return *this;
    }

    route_mesh_channel_builder_t &set_default_request_timeout (std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::milliseconds::zero ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "route mesh request timeout must be greater than zero");
        }
        _default_request_timeout = timeout;
        apply ();
        return *this;
    }

    route_mesh_channel_builder_t &use_handler_group (std::string group_name)
    {
        detail::require_non_blank (group_name, "handler group name is required");
        _handler_groups->add_channel (
          group_name, _channel_name,
          {detail::handler_group_kind_t::request, detail::handler_group_kind_t::send},
          "route mesh channel");
        _route_handler_groups.push_back (std::move (group_name));
        apply ();
        return *this;
    }

    template <typename TOwner, typename TMessage>
    route_mesh_channel_builder_t &
    add_send_handler (std::string packet_name,
                      void (TOwner::*method) (const TMessage &, const route_message_context_t &))
    {
        _route_handlers.push_back (
          [packet = std::move (packet_name), method] (route_channel_builder_t &channel) mutable {
              channel.add_send_handler<TOwner, TMessage> (std::move (packet), method);
          });
        apply ();
        return *this;
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_mesh_channel_builder_t &add_request_handler (
      std::string packet_name,
      TReply (TOwner::*method) (const TRequest &, const route_message_context_t &))
    {
        _route_handlers.push_back (
          [packet = std::move (packet_name), method] (route_channel_builder_t &channel) mutable {
              channel.add_request_handler<TOwner, TRequest, TReply> (std::move (packet), method);
          });
        apply ();
        return *this;
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_mesh_channel_builder_t &
    add_request_handler (std::string packet_name,
                         task_t<TReply> (TOwner::*method) (const TRequest &))
    {
        _route_handlers.push_back (
          [packet = std::move (packet_name), method] (route_channel_builder_t &channel) mutable {
              channel.add_request_handler<TOwner, TRequest, TReply> (std::move (packet), method);
          });
        apply ();
        return *this;
    }

  private:
    void apply ()
    {
        const auto channel_name = _channel_name;
        const auto bind_endpoint = _bind_endpoint;
        const auto routing_id = _routing_id;
        const auto manual_connections = _manual_connections;
        const auto default_request_timeout = _default_request_timeout;
        const auto route_handler_groups = _route_handler_groups;
        const auto route_handlers = _route_handlers;
        _options->set_zlink_action (
          "route_mesh_channel:" + channel_name,
          [channel_name, bind_endpoint, routing_id, manual_connections, default_request_timeout,
           route_handler_groups, route_handlers] (zlink_builder_t &zlink) {
              auto channel = zlink.route_channel (channel_name);
              if (!bind_endpoint.empty ()) {
                  channel.bind (bind_endpoint);
              }
              if (routing_id) {
                  channel.set_routing_id (*routing_id);
              }
              if (default_request_timeout) {
                  channel.default_request_timeout (*default_request_timeout);
              }
              for (const auto &endpoint : manual_connections) {
                  channel.connect (endpoint);
              }
              for (const auto &group : route_handler_groups) {
                  channel.add_handler_group (group);
              }
              for (const auto &handler : route_handlers) {
                  handler (channel);
              }
          });
    }

    std::string _channel_name;
    std::shared_ptr<detail::framework_options_state_t> _options;
    std::shared_ptr<detail::handler_group_options_state_t> _handler_groups;
    std::string _bind_endpoint;
    std::optional<zlink::routing_id_t> _routing_id;
    std::optional<std::chrono::milliseconds> _default_request_timeout;
    std::vector<std::string> _manual_connections;
    std::vector<std::string> _route_handler_groups;
    std::vector<std::function<void (route_channel_builder_t &)>> _route_handlers;
};

struct stream_socket_config_t
{
    // The complete header plus payload size accepted from a client.
    // Zero disables the separate Framework cap.
    std::int64_t max_message_size = 64 * 1024;
};

class stream_node_options_builder_t
{
  public:
    stream_node_options_builder_t (std::string stream_name,
                                   service_collection_t &services,
                                   std::shared_ptr<detail::framework_options_state_t> options) :
        _stream_name (std::move (stream_name)),
        _services (&services),
        _options (std::move (options))
    {
        detail::require_non_blank (_stream_name, "STREAM node name is required");
        if (!_options->stream_nodes.insert (_stream_name).second) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM node '" + _stream_name
                                           + "' is already registered");
        }
    }

    stream_node_options_builder_t &bind (std::string endpoint)
    {
        detail::require_non_blank (endpoint, "STREAM bind endpoint is required");
        _endpoint = std::move (endpoint);
        _options->stream_nodes_with_bind.insert (_stream_name);
        apply ();
        return *this;
    }

    stream_node_options_builder_t &set_tls_server (
      std::string certificate_file,
      std::string private_key_file,
      bool require_client_certificate = false)
    {
        detail::require_non_blank (certificate_file, "STREAM TLS certificate file is required");
        detail::require_non_blank (private_key_file, "STREAM TLS private key file is required");
        _tls_certificate_file = std::move (certificate_file);
        _tls_private_key_file = std::move (private_key_file);
        _tls_require_client_certificate = require_client_certificate;
        apply ();
        return *this;
    }

    stream_socket_config_t &configure_socket () noexcept { return _socket_config; }

    //  Lets a STREAM session relay to Actors. Mirrors the .NET
    //  EnableActorDispatch() and the Java/Node enableActorDispatch(), including
    //  the duplicate-call rejection.
    stream_node_options_builder_t &enable_actor_dispatch ()
    {
        if (!_options->stream_nodes_with_actor_dispatch.insert (_stream_name).second) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM node '" + _stream_name
                                           + "' already enabled actor dispatch");
        }
        return *this;
    }

    stream_node_options_builder_t &register_session (std::string session_name)
    {
        detail::require_non_blank (session_name, "STREAM packet session name is required");
        set_session_name (std::move (session_name));
        apply ();
        return *this;
    }

    template <typename TSession>
    requires std::derived_from<TSession, packet_stream_session_t> stream_node_options_builder_t &
    register_session ()
    {
        auto session_name = detail::stream_session_name<TSession> ();
        set_session_name (session_name);
        detail::injected_stream_session_registrar_t<
          TSession, typename detail::handler_dependencies_t<TSession>::type>::add (*_services);
        _options->stream_session_factories[session_name] =
          [] (service_provider_t &provider) -> packet_stream_session_t & {
            return provider.get_required<TSession> ();
        };
        apply ();
        return *this;
    }

  private:
    void set_session_name (std::string session_name)
    {
        if (_session_configured) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "stream node already has a packet session");
        }
        if (!_options->stream_session_names.insert (session_name).second) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM packet session '" + session_name
                                           + "' is already registered");
        }
        _session_configured = true;
        _session_name = std::move (session_name);
        _options->stream_nodes_with_session.insert (_stream_name);
    }

    void apply ()
    {
        if (_endpoint.empty () || _session_name.empty ()) {
            return;
        }
        if (_socket_config.max_message_size < 0) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "STREAM MaxMessageSize must be zero or positive");
        }
        const auto stream_name = _stream_name;
        const auto endpoint = _endpoint;
        const auto session_name = _session_name;
        const auto max_message_size = _socket_config.max_message_size;
        const auto tls_certificate_file = _tls_certificate_file;
        const auto tls_private_key_file = _tls_private_key_file;
        const auto tls_require_client_certificate = _tls_require_client_certificate;
        _options->set_zlink_action (
          "stream_node:" + stream_name,
          [stream_name, endpoint, session_name, max_message_size, tls_certificate_file,
           tls_private_key_file, tls_require_client_certificate] (zlink_builder_t &zlink) {
              auto stream = zlink.stream (stream_name);
              stream.set_max_message_size (max_message_size);
              if (!endpoint.empty ()) {
                  stream.bind (endpoint);
              }
              if (!tls_certificate_file.empty () || !tls_private_key_file.empty ()) {
                  stream.configure_tls_server (tls_certificate_file, tls_private_key_file,
                                               tls_require_client_certificate);
              }
              if (!session_name.empty ()) {
                  stream.register_session (session_name);
              }
          });
    }

    std::string _stream_name;
    service_collection_t *_services;
    std::shared_ptr<detail::framework_options_state_t> _options;
    std::string _endpoint;
    std::string _session_name;
    std::string _tls_certificate_file;
    std::string _tls_private_key_file;
    stream_socket_config_t _socket_config;
    bool _tls_require_client_certificate = false;
    bool _session_configured = false;
};

class stream_compression_options_builder_t
{
  public:
    explicit stream_compression_options_builder_t (
      std::shared_ptr<detail::framework_options_state_t> options) :
        _options (std::move (options))
    {
    }

    stream_compression_options_builder_t &use_default () { return use_lz4 (); }

    stream_compression_options_builder_t &use_lz4 ()
    {
        return use (lz4_stream_compression_codec ());
    }

    stream_compression_options_builder_t &
    use (std::shared_ptr<const stream_compression_codec_t> codec)
    {
        if (!codec) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "STREAM compression codec must not be null");
        }
        set (std::move (codec));
        return *this;
    }

    stream_compression_options_builder_t &disable ()
    {
        set (nullptr);
        return *this;
    }

  private:
    void set (std::shared_ptr<const stream_compression_codec_t> codec)
    {
        _options->set_zlink_action (
          "stream_compression", [codec = std::move (codec)] (zlink_builder_t &zlink) mutable {
              detail::apply_stream_compression_codec (zlink, std::move (codec));
          });
    }

    std::shared_ptr<detail::framework_options_state_t> _options;
};

class zlink_framework_options_t
{
  public:
    zlink_framework_options_t (service_collection_t &services,
                               handler_registry_t &handlers,
                               serializer_registry_t &serializers,
                               zlink_builder_t &zlink) :
        _services (&services),
        _handlers (&handlers),
        _serializers (&serializers),
        _zlink (&zlink),
        _handler_groups (std::make_shared<detail::handler_group_options_state_t> ()),
        _options (std::make_shared<detail::framework_options_state_t> ())
    {
        _inbound_dispatch.emplace (_options);
        _options->http.bind_services (services, serializers);
        if (!services.contains (std::type_index (typeid (message_metadata_policy_t)))) {
            auto options = _options;
            services.add_factory<message_metadata_policy_t> (
              [options] (service_provider_t &) {
                  return std::make_unique<message_metadata_policy_t> (options->metadata_policy);
              },
              service_lifetime_t::singleton);
        }
    }

    handler_options_builder_t handlers ()
    {
        return handler_options_builder_t (*_services, *_handlers, *_serializers, _handler_groups);
    }

    codec_options_builder_t codecs () { return codec_options_builder_t (*_serializers); }

    metadata_policy_builder_t metadata () { return metadata_policy_builder_t (_options); }

    dispatch_options_t &configure_dispatch () { return _options->dispatch; }

    dispatch_options_t dispatch_options () const { return _options->dispatch; }

    worker_options_t &worker () { return _options->worker; }

    inbound_dispatch_options_t &configure_inbound_dispatch ()
    {
        return *_inbound_dispatch;
    }

    std::optional<std::uint64_t> application_hwm_bytes () const noexcept
    {
        return _options->application_hwm_bytes;
    }

    application_hwm_profile_t application_hwm_profile () const noexcept
    {
        return _options->application_hwm_profile;
    }

    std::optional<std::uint64_t> process_memory_limit_bytes () const noexcept
    {
        return _options->process_memory_limit_bytes;
    }

    location_options_t &configure_locations () { return _options->locations; }

    location_options_t location_options () const { return _options->locations; }

    const std::set<std::string> &route_mesh_client_channels () const noexcept
    {
        return _options->route_mesh_channels_with_client;
    }

    zlink_framework_options_t &
    set_message_follow_duration (std::chrono::milliseconds duration)
    {
        if (duration < std::chrono::milliseconds::zero ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "Message Follow duration must not be negative");
        }
        _options->locations.message_follow_duration = duration;
        return *this;
    }

    zlink_framework_options_t &set_default_request_timeout (std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::milliseconds::zero ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "request timeout must be greater than zero");
        }
        _options->set_zlink_action ("default_request_timeout", [timeout] (zlink_builder_t &zlink) {
            zlink.default_request_timeout (timeout);
        });
        return *this;
    }

    service_collection_t &services () noexcept { return *_services; }

    zlink_framework_options_t &add_location_store (std::shared_ptr<location_store_t> store)
    {
        if (!store) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "location store instance must not be null");
        }
        if (_options->has_location_store_instance) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "location store must be registered exactly once");
        }
        _options->has_location_store_instance = true;
        register_location_store_instance (std::move (store));
        return *this;
    }

    zlink_framework_options_t &
    add_relocation_store (std::shared_ptr<relocation_store_t> store)
    {
        if (!store) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "relocation store instance must not be null");
        }
        if (_options->has_relocation_store_instance) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "relocation store must be registered exactly once");
        }
        _options->has_relocation_store_instance = true;
        _services->add_factory<relocation_store_t> (
          [store] (service_provider_t &) { return store; },
          service_lifetime_t::singleton);
        return *this;
    }

    client_server_channel_builder_t add_client_server_channel (std::string channel_name)
    {
        return client_server_channel_builder_t (std::move (channel_name), _options,
                                                _handler_groups, *_services,
                                                *_handlers, *_serializers);
    }

    fanout_channel_builder_t add_fanout_channel (std::string channel_name)
    {
        return fanout_channel_builder_t (std::move (channel_name), _options, _handler_groups);
    }

    mesh_node_builder_t add_route_mesh (std::string mesh_name);

    stream_node_options_builder_t add_stream_node (std::string stream_name)
    {
        return stream_node_options_builder_t (std::move (stream_name), *_services, _options);
    }

    stream_compression_options_builder_t configure_stream_compression ()
    {
        return stream_compression_options_builder_t (_options);
    }

    http_options_builder_t &http () noexcept { return _options->http; }

    const std::map<std::string, detail::stream_session_factory_t> &
    stream_session_factories () const noexcept
    {
        return _options->stream_session_factories;
    }

    /* Live endpoint handles per manual role (endpoint_connections contract):
     * the host attaches runtime connect/disconnect appliers after apply(). */
    std::map<std::string, endpoint_connections_t> &client_endpoint_connections () const noexcept
    {
        return _options->client_endpoint_connections;
    }

    const std::map<std::string, std::string> &
    client_server_server_advertise_hosts () const noexcept
    {
        return _options->client_server_server_advertise_hosts;
    }

    std::map<std::string, endpoint_connections_t> &
    subscriber_endpoint_connections () const noexcept
    {
        return _options->subscriber_endpoint_connections;
    }

    template <typename TFilter> zlink_framework_options_t &use_filter ()
    {
        detail::injected_handler_registrar_t<
          TFilter, typename detail::handler_dependencies_t<TFilter>::type>::add (*_services);
        _handlers->use_filter<TFilter> ();
        return *this;
    }

    zlink_framework_options_t &handler_coroutine_workers (std::size_t worker_count)
    {
        _handler_coroutine_workers = worker_count;
        return *this;
    }

    std::size_t handler_coroutine_workers () const noexcept { return _handler_coroutine_workers; }

    void apply ()
    {
        if (_options->applied) {
            return;
        }
        _options->http.validate ();
        detail::validate_framework_options (*_options, *_handler_groups);
        _options->worker.seal ();
        detail::apply_dispatch_options (*_zlink, _options->dispatch);
        try {
            for (const auto &[_, action] : _options->keyed_zlink_actions) {
                action (*_zlink);
            }
            _handler_groups->install_route_handlers (*_zlink);
            for (const auto &action : _options->deferred_zlink_actions) {
                action (*_zlink);
            }
        }
        catch (...) {
            throw;
        }
        _options->applied = true;
    }

  private:
    void register_location_store_instance (std::shared_ptr<location_store_t> store)
    {
        _services->add_factory<location_store_t> (
          [store] (service_provider_t &) {
              return std::static_pointer_cast<location_store_t> (store);
          },
          service_lifetime_t::singleton);
    }

    service_collection_t *_services;
    handler_registry_t *_handlers;
    serializer_registry_t *_serializers;
    zlink_builder_t *_zlink;
    std::shared_ptr<detail::handler_group_options_state_t> _handler_groups;
    std::shared_ptr<detail::framework_options_state_t> _options;
    std::optional<inbound_dispatch_options_t> _inbound_dispatch;
    std::size_t _handler_coroutine_workers = 0;
};

} // namespace zlink::framework
