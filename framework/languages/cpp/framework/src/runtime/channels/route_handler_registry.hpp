/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace zlink::framework::detail
{

struct route_handler_descriptor_t
{
    runtime::messaging::message_kind_t kind;
    std::string router_channel_id;
    std::string packet_name;
    std::type_index owner_type;
    std::type_index message_type;
    std::type_index reply_type;
};

class route_handler_registry_t
{
  public:
    using invoker_t =
      std::function<task_t<zlink::message_t> (service_provider_t &,
                                              serializer_registry_t &,
                                              const zlink::message_t &,
                                              const framework::route_message_context_t &,
                                              detail::handler_start_callback_ref_t)>;

    struct entry_t
    {
        route_handler_descriptor_t descriptor;
        invoker_t invoker;
    };

    const entry_t *resolve (std::string_view router_channel_id,
                            runtime::messaging::message_kind_t kind,
                            std::string_view packet_name) const;

    template <typename TOwner, typename TMessage>
    route_handler_registry_t &
    on_send (std::string router_channel_id,
             std::string packet_name,
             void (TOwner::*method) (const TMessage &, const framework::route_message_context_t &))
    {
        const auto packet = packet_name.empty () ? framework::detail::message_name<TMessage> ()
                                                 : std::move (packet_name);
        return add_handler (
          route_handler_descriptor_t{
            runtime::messaging::message_kind_t::command, std::move (router_channel_id), packet,
            std::type_index (typeid (TOwner)), std::type_index (typeid (TMessage)),
            std::type_index (typeid (void))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const framework::route_message_context_t &context,
                    detail::handler_start_callback_ref_t before_application_handler)
            -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = detail::deserialize_typed_payload<TMessage> (
                    serializers, message, context.content_type.value_or (""));
                  detail::invoke_prepared_handler (before_application_handler, method, owner,
                                                   payload, context);
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (zlink::message_t{}));
              }
              catch (...) {
                  return task_t<zlink::message_t> (current_exception_result<zlink::message_t> ());
              }
          });
    }

    template <typename TOwner, typename TMessage>
    route_handler_registry_t &on_send (
      std::string router_channel_id,
      std::string packet_name,
      task_t<void> (TOwner::*method) (const TMessage &, const framework::route_message_context_t &))
    {
        const auto packet = packet_name.empty () ? framework::detail::message_name<TMessage> ()
                                                 : std::move (packet_name);
        return add_handler (
          route_handler_descriptor_t{
            runtime::messaging::message_kind_t::command, std::move (router_channel_id), packet,
            std::type_index (typeid (TOwner)), std::type_index (typeid (TMessage)),
            std::type_index (typeid (void))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const framework::route_message_context_t &context,
                    detail::handler_start_callback_ref_t before_application_handler)
            -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = detail::deserialize_typed_payload<TMessage> (
                    serializers, message, context.content_type.value_or (""));
                  co_await detail::invoke_prepared_handler (before_application_handler, method,
                                                            owner, payload, context);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              }
              catch (...) {
                  co_return current_exception_result<zlink::message_t> ();
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_handler_registry_t &on_request (
      std::string router_channel_id,
      std::string packet_name,
      TReply (TOwner::*method) (const TRequest &, const framework::route_message_context_t &))
    {
        const auto packet = packet_name.empty () ? framework::detail::message_name<TRequest> ()
                                                 : std::move (packet_name);
        return add_handler (
          route_handler_descriptor_t{
            runtime::messaging::message_kind_t::request, std::move (router_channel_id), packet,
            std::type_index (typeid (TOwner)), std::type_index (typeid (TRequest)),
            std::type_index (typeid (TReply))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const framework::route_message_context_t &context,
                    detail::handler_start_callback_ref_t before_application_handler)
            -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = detail::deserialize_typed_payload<TRequest> (
                    serializers, message, context.content_type.value_or (""));
                  return serialize_handler_result (
                    detail::invoke_prepared_handler (before_application_handler, method, owner,
                                                     request, context),
                    serializers);
              }
              catch (...) {
                  return task_t<zlink::message_t> (current_exception_result<zlink::message_t> ());
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    route_handler_registry_t &
    on_request (std::string router_channel_id,
                std::string packet_name,
                task_t<TReply> (TOwner::*method) (const TRequest &,
                                                  const framework::route_message_context_t &))
    {
        const auto packet = packet_name.empty () ? framework::detail::message_name<TRequest> ()
                                                 : std::move (packet_name);
        return add_handler (
          route_handler_descriptor_t{
            runtime::messaging::message_kind_t::request, std::move (router_channel_id), packet,
            std::type_index (typeid (TOwner)), std::type_index (typeid (TRequest)),
            std::type_index (typeid (TReply))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const framework::route_message_context_t &context,
                    detail::handler_start_callback_ref_t before_application_handler)
            -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = detail::deserialize_typed_payload<TRequest> (
                    serializers, message, context.content_type.value_or (""));
                  auto reply = co_await detail::invoke_prepared_handler (
                    before_application_handler, method, owner, request, context);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              }
              catch (...) {
                  co_return current_exception_result<zlink::message_t> ();
              }
          });
    }

    route_handler_registry_t &add_handler (route_handler_descriptor_t descriptor,
                                           invoker_t invoker);

    const route_handler_descriptor_t *find (std::string_view router_channel_id,
                                            runtime::messaging::message_kind_t kind,
                                            std::string_view packet_name) const;


  private:
    struct key_view_t
    {
        std::string_view router_channel_id;
        runtime::messaging::message_kind_t kind;
        std::string_view packet_name;
    };

    struct key_t
    {
        std::string router_channel_id;
        runtime::messaging::message_kind_t kind;
        std::string packet_name;

        operator key_view_t () const noexcept { return {router_channel_id, kind, packet_name}; }
    };

    struct key_hash_t
    {
        using is_transparent = void;

        std::size_t operator() (key_view_t key) const noexcept
        {
            auto seed = std::hash<std::string_view>{}(key.router_channel_id);
            constexpr std::size_t hash_mix_constant = 0x9e3779b9U;
            for (const auto value : {std::hash<int>{}(static_cast<int> (key.kind)),
                                     std::hash<std::string_view>{}(key.packet_name)}) {
                seed ^= value + hash_mix_constant + (seed << 6U) + (seed >> 2U);
            }
            return seed;
        }
    };

    struct key_equal_t
    {
        using is_transparent = void;

        bool operator() (key_view_t left, key_view_t right) const noexcept
        {
            return left.router_channel_id == right.router_channel_id && left.kind == right.kind
                   && left.packet_name == right.packet_name;
        }
    };

    std::unordered_map<key_t, entry_t, key_hash_t, key_equal_t> _handlers;
};

} // namespace zlink::framework::detail
