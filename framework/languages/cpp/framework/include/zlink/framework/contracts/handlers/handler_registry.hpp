/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/messaging/message_context.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <utility>

namespace zlink::framework
{

namespace detail
{
class handler_registry_state_t;
class route_handler_invoker_t;
} // namespace detail

enum class handler_kind_t
{
    request = 0,
    send = 1,
    event = 2,
    raw = 3
};

struct handler_options_t
{
    std::optional<std::string> packet_name;
    handler_execution_t execution = handler_execution_t::inline_on_runtime;
};

struct handler_descriptor_t
{
    std::string channel_name;
    std::string topic;
    std::string packet_name;
    handler_kind_t kind;
    handler_execution_t execution;
    std::type_index owner_type;
    std::type_index payload_type;
};

struct handler_failure_event_t
{
    handler_descriptor_t descriptor;
    framework_error_kind_t error_kind;
    std::string message;
};

class payload_view_t
{
  public:
    explicit payload_view_t (encoded_payload_t payload) : _payload (std::move (payload)) {}

    std::span<const std::byte> bytes () const noexcept { return _payload.bytes (); }
    std::vector<std::uint8_t> to_bytes () const { return _payload.to_bytes (); }
    std::string to_string () const { return _payload.to_string (); }

  private:
    encoded_payload_t _payload;
};

/// Continues the current dispatch pipeline. A filter may call it at most once.
using handler_next_t = std::function<task_t<void> ()>;

class handler_registry_t
{
  public:
    using raw_handler_t = std::function<result_t<void> (const payload_view_t &)>;
    using invoker_t =
      std::function<task_t<zlink::message_t> (service_provider_t &,
                                              serializer_registry_t &,
                                              const zlink::message_t &,
                                              const detail::inbound_message_context_t &)>;
    using failure_observer_t = std::function<void (const handler_failure_event_t &)>;
    using filter_invoker_t = std::function<task_t<void> (service_provider_t &,
                                                          serializer_registry_t &,
                                                          const handler_filter_context_t &,
                                                          handler_next_t)>;

    handler_registry_t ();
    ~handler_registry_t ();

    handler_registry_t (handler_registry_t &&) noexcept;
    handler_registry_t &operator= (handler_registry_t &&) noexcept;
    handler_registry_t (const handler_registry_t &) = delete;
    handler_registry_t &operator= (const handler_registry_t &) = delete;

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &on_request (std::string channel_name,
                                    std::string topic,
                                    TReply (TOwner::*method) (const TRequest &),
                                    handler_options_t options = {})
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TRequest> ());
        return add_handler (
          {std::move (channel_name), std::move (topic), packet, handler_kind_t::request,
           options.execution, std::type_index (typeid (TOwner)),
           std::type_index (typeid (TRequest))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = (owner.*method) (request);
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply))));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    detail::current_exception_to_message_result ("handler threw an exception"));
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &on_request (std::string channel_name,
                                    std::string topic,
                                    task_t<TReply> (TOwner::*method) (const TRequest &),
                                    handler_options_t options = {})
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TRequest> ());
        return add_handler (
          {std::move (channel_name), std::move (topic), packet, handler_kind_t::request,
           options.execution, std::type_index (typeid (TOwner)),
           std::type_index (typeid (TRequest))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = co_await (owner.*method) (request);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              }
              catch (...) {
                  co_return detail::current_exception_to_message_result (
                    "handler threw an exception");
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &on_request (std::string channel_name,
                                    std::string topic,
                                    TReply (TOwner::*method) (const TRequest &,
                                                              const message_context_t &),
                                    handler_options_t options = {})
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TRequest> ());
        handler_descriptor_t descriptor{std::move (channel_name),
                                        std::move (topic),
                                        packet,
                                        handler_kind_t::request,
                                        options.execution,
                                        std::type_index (typeid (TOwner)),
                                        std::type_index (typeid (TRequest))};
        return add_handler (
          std::move (descriptor),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &inbound) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = (owner.*method) (request, inbound.message);
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply))));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    detail::current_exception_to_message_result ("handler threw an exception"));
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &on_request (std::string channel_name,
                                    std::string topic,
                                    task_t<TReply> (TOwner::*method) (const TRequest &,
                                                                      const message_context_t &),
                                    handler_options_t options = {})
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TRequest> ());
        handler_descriptor_t descriptor{std::move (channel_name),
                                        std::move (topic),
                                        packet,
                                        handler_kind_t::request,
                                        options.execution,
                                        std::type_index (typeid (TOwner)),
                                        std::type_index (typeid (TRequest))};
        return add_handler (
          std::move (descriptor),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &inbound) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto request = serializers.get<TRequest> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  auto reply = co_await (owner.*method) (request, inbound.message);
                  co_return result_t<zlink::message_t>::success (
                    detail::encoded_payload_to_raw (serializers.get<TReply> ().serialize (reply)));
              }
              catch (...) {
                  co_return detail::current_exception_to_message_result (
                    "handler threw an exception");
              }
          });
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &request (std::string channel_name,
                                 std::string topic,
                                 TReply (TOwner::*method) (const TRequest &),
                                 handler_options_t options = {})
    {
        return on_request<TOwner, TRequest, TReply> (std::move (channel_name), std::move (topic),
                                                     method, std::move (options));
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &request (std::string channel_name,
                                 std::string topic,
                                 task_t<TReply> (TOwner::*method) (const TRequest &),
                                 handler_options_t options = {})
    {
        return on_request<TOwner, TRequest, TReply> (std::move (channel_name), std::move (topic),
                                                     method, std::move (options));
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &request (std::string channel_name,
                                 std::string topic,
                                 TReply (TOwner::*method) (const TRequest &,
                                                           const message_context_t &),
                                 handler_options_t options = {})
    {
        return on_request<TOwner, TRequest, TReply> (std::move (channel_name), std::move (topic),
                                                     method, std::move (options));
    }

    template <typename TOwner, typename TRequest, typename TReply>
    handler_registry_t &request (std::string channel_name,
                                 std::string topic,
                                 task_t<TReply> (TOwner::*method) (const TRequest &,
                                                                   const message_context_t &),
                                 handler_options_t options = {})
    {
        return on_request<TOwner, TRequest, TReply> (std::move (channel_name), std::move (topic),
                                                     method, std::move (options));
    }

    template <typename TOwner, typename TMessage>
    handler_registry_t &on_send (std::string channel_name,
                                 std::string topic,
                                 void (TOwner::*method) (const TMessage &),
                                 handler_options_t options = {})
    {
        return add_void_member_handler<TOwner, TMessage> (std::move (channel_name),
                                                          std::move (topic), handler_kind_t::send,
                                                          method, std::move (options));
    }

    template <typename TOwner, typename TMessage>
    handler_registry_t &on_send (std::string channel_name,
                                 std::string topic,
                                 task_t<void> (TOwner::*method) (const TMessage &),
                                 handler_options_t options = {})
    {
        return add_task_member_handler<TOwner, TMessage> (std::move (channel_name),
                                                          std::move (topic), handler_kind_t::send,
                                                          method, std::move (options));
    }

    template <typename TOwner, typename TMessage>
    handler_registry_t &on_send (std::string channel_name,
                                 std::string topic,
                                 void (TOwner::*method) (const TMessage &, const message_context_t &),
                                 handler_options_t options = {})
    {
        return add_context_void_member_handler<TOwner, TMessage, message_context_t> (
          std::move (channel_name), std::move (topic), handler_kind_t::send, method,
          std::move (options));
    }

    template <typename TOwner, typename TMessage>
    handler_registry_t &on_send (std::string channel_name,
                                 std::string topic,
                                 task_t<void> (TOwner::*method) (const TMessage &,
                                                                 const message_context_t &),
                                 handler_options_t options = {})
    {
        return add_context_task_member_handler<TOwner, TMessage, message_context_t> (
          std::move (channel_name), std::move (topic), handler_kind_t::send, method,
          std::move (options));
    }

    template <typename TOwner, typename TEvent>
    handler_registry_t &on_event (std::string channel_name,
                                  std::string topic,
                                  void (TOwner::*method) (const TEvent &),
                                  handler_options_t options = {})
    {
        return add_void_member_handler<TOwner, TEvent> (std::move (channel_name), std::move (topic),
                                                        handler_kind_t::event, method,
                                                        std::move (options));
    }

    template <typename TOwner, typename TEvent>
    handler_registry_t &on_event (std::string channel_name,
                                  std::string topic,
                                  task_t<void> (TOwner::*method) (const TEvent &),
                                  handler_options_t options = {})
    {
        return add_task_member_handler<TOwner, TEvent> (std::move (channel_name), std::move (topic),
                                                        handler_kind_t::event, method,
                                                        std::move (options));
    }

    template <typename TOwner, typename TEvent>
    handler_registry_t &on_event (std::string channel_name,
                                  std::string topic,
                                  void (TOwner::*method) (const TEvent &,
                                                          const publish_message_context_t &),
                                  handler_options_t options = {})
    {
        return add_context_void_member_handler<TOwner, TEvent, publish_message_context_t> (
          std::move (channel_name), std::move (topic), handler_kind_t::event, method,
          std::move (options));
    }

    template <typename TOwner, typename TEvent>
    handler_registry_t &on_event (std::string channel_name,
                                  std::string topic,
                                  task_t<void> (TOwner::*method) (const TEvent &,
                                                                  const publish_message_context_t &),
                                  handler_options_t options = {})
    {
        return add_context_task_member_handler<TOwner, TEvent, publish_message_context_t> (
          std::move (channel_name), std::move (topic), handler_kind_t::event, method,
          std::move (options));
    }

    handler_registry_t &send_raw (std::string channel_name,
                                  std::string packet_name,
                                  raw_handler_t handler,
                                  handler_options_t options = {});
    handler_registry_t &send_raw (std::string channel_name,
                                  std::string topic,
                                  std::string packet_name,
                                  raw_handler_t handler,
                                  handler_options_t options = {});
    template <typename TFilter> handler_registry_t &use_filter ()
    {
        return add_filter ([] (service_provider_t &services, serializer_registry_t &,
                               const handler_filter_context_t &context,
                               handler_next_t next) -> task_t<void> {
            auto &filter = services.get_required<TFilter> ();
            return filter.invoke (context, std::move (next));
        });
    }

    handler_registry_t &add_filter (filter_invoker_t filter);
    handler_registry_t &observe_failures (failure_observer_t observer);

    const handler_descriptor_t *find (std::string_view channel_name,
                                      std::string_view packet_name) const;
    const handler_descriptor_t *find (std::string_view channel_name,
                                      std::string_view topic,
                                      std::string_view packet_name) const;
    result_t<zlink::message_t> invoke (std::string_view channel_name,
                                       std::string_view packet_name,
                                       service_provider_t &services,
                                       serializer_registry_t &serializers,
                                       const zlink::message_t &message,
                                       const detail::inbound_message_context_t &inbound = {}) const;
    result_t<zlink::message_t> invoke (std::string_view channel_name,
                                       std::string_view topic,
                                       std::string_view packet_name,
                                       service_provider_t &services,
                                       serializer_registry_t &serializers,
                                       const zlink::message_t &message,
                                       const detail::inbound_message_context_t &inbound = {}) const;

  private:
    using terminal_invoker_t = std::function<task_t<zlink::message_t> ()>;

    task_t<zlink::message_t>
    invoke_filters_async (handler_dispatch_kind_t dispatch_kind,
                          service_provider_t &services,
                          serializer_registry_t &serializers,
                          const message_context_t &context,
                          terminal_invoker_t terminal) const;

    task_t<zlink::message_t> invoke_async (std::string_view channel_name,
                                           std::string_view packet_name,
                                           service_provider_t &services,
                                           serializer_registry_t &serializers,
                                           const zlink::message_t &message,
                                           const detail::inbound_message_context_t &inbound = {}) const;
    task_t<zlink::message_t> invoke_async (std::string_view channel_name,
                                           std::string_view topic,
                                           std::string_view packet_name,
                                           service_provider_t &services,
                                           serializer_registry_t &serializers,
                                           const zlink::message_t &message,
                                           const detail::inbound_message_context_t &inbound = {}) const;

    template <typename TOwner, typename TPayload>
    handler_registry_t &add_void_member_handler (std::string channel_name,
                                                 std::string topic,
                                                 handler_kind_t kind,
                                                 void (TOwner::*method) (const TPayload &),
                                                 handler_options_t options)
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TPayload> ());
        return add_handler (
          {std::move (channel_name), std::move (topic), packet, kind, options.execution,
           std::type_index (typeid (TOwner)), std::type_index (typeid (TPayload))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TPayload> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  (owner.*method) (payload);
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (zlink::message_t{}));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    detail::current_exception_to_message_result ("handler threw an exception"));
              }
          });
    }

    template <typename TOwner, typename TPayload>
    handler_registry_t &add_task_member_handler (std::string channel_name,
                                                 std::string topic,
                                                 handler_kind_t kind,
                                                 task_t<void> (TOwner::*method) (const TPayload &),
                                                 handler_options_t options)
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TPayload> ());
        return add_handler (
          {std::move (channel_name), std::move (topic), packet, kind, options.execution,
           std::type_index (typeid (TOwner)), std::type_index (typeid (TPayload))},
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TPayload> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  co_await (owner.*method) (payload);
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              }
              catch (...) {
                  co_return detail::current_exception_to_message_result (
                    "handler threw an exception");
              }
          });
    }

    /// Projects the exact context type the handler declared out of the inbound carrier so the
    /// public context structs never have to be downcast.
    template <typename TContext>
    static TContext project_context (const detail::inbound_message_context_t &inbound)
    {
        if constexpr (std::is_same_v<TContext, publish_message_context_t>) {
            return inbound.as_publish_context ();
        } else {
            return inbound.message;
        }
    }

    template <typename TOwner, typename TPayload, typename TContext>
    handler_registry_t &add_context_void_member_handler (std::string channel_name,
                                                         std::string topic,
                                                         handler_kind_t kind,
                                                         void (TOwner::*method) (const TPayload &,
                                                                                 const TContext &),
                                                         handler_options_t options)
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TPayload> ());
        handler_descriptor_t descriptor{std::move (channel_name),
                                        std::move (topic),
                                        packet,
                                        kind,
                                        options.execution,
                                        std::type_index (typeid (TOwner)),
                                        std::type_index (typeid (TPayload))};
        return add_handler (
          std::move (descriptor),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &inbound) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TPayload> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  (owner.*method) (payload, project_context<TContext> (inbound));
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (zlink::message_t{}));
              }
              catch (...) {
                  return task_t<zlink::message_t> (
                    detail::current_exception_to_message_result ("handler threw an exception"));
              }
          });
    }

    template <typename TOwner, typename TPayload, typename TContext>
    handler_registry_t &add_context_task_member_handler (
      std::string channel_name,
      std::string topic,
      handler_kind_t kind,
      task_t<void> (TOwner::*method) (const TPayload &, const TContext &),
      handler_options_t options)
    {
        const auto packet = options.packet_name.value_or (detail::message_name<TPayload> ());
        handler_descriptor_t descriptor{std::move (channel_name),
                                        std::move (topic),
                                        packet,
                                        kind,
                                        options.execution,
                                        std::type_index (typeid (TOwner)),
                                        std::type_index (typeid (TPayload))};
        return add_handler (
          std::move (descriptor),
          [method] (service_provider_t &services, serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const detail::inbound_message_context_t &inbound) -> task_t<zlink::message_t> {
              try {
                  auto &owner = services.get_required<TOwner> ();
                  auto payload = serializers.get<TPayload> ().deserialize (
                    detail::encoded_payload_from_raw (message));
                  co_await (owner.*method) (payload, project_context<TContext> (inbound));
                  co_return result_t<zlink::message_t>::success (zlink::message_t{});
              }
              catch (...) {
                  co_return detail::current_exception_to_message_result (
                    "handler threw an exception");
              }
          });
    }

    handler_registry_t &add_handler (handler_descriptor_t descriptor, invoker_t invoker);
    void emit_failure (const handler_descriptor_t &descriptor,
                       const framework_exception_t &error) const;

    friend class detail::route_handler_invoker_t;

    std::unique_ptr<detail::handler_registry_state_t> _state;
};

} // namespace zlink::framework
