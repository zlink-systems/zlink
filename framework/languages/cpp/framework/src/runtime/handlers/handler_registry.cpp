/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/handlers/handler_registry.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework
{

namespace
{

std::mutex &handler_invocation_executor_mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<runtime::offload_executor_t> &handler_invocation_executor_ref ()
{
    static std::shared_ptr<runtime::offload_executor_t> executor;
    return executor;
}

std::shared_ptr<runtime::offload_executor_t> handler_invocation_executor ()
{
    std::lock_guard lock (handler_invocation_executor_mutex ());
    if (!handler_invocation_executor_ref ()) {
        handler_invocation_executor_ref () = std::make_shared<runtime::offload_executor_t> (
          0, std::max<std::size_t> (1, std::thread::hardware_concurrency ()), 4096,
          std::chrono::milliseconds (100), "zlink-handler");
    }
    return handler_invocation_executor_ref ();
}

void ensure_handler_invocation_executor ()
{
    (void) handler_invocation_executor ();
}

handler_dispatch_kind_t dispatch_kind_for (handler_kind_t kind)
{
    switch (kind) {
        case handler_kind_t::request:
            return handler_dispatch_kind_t::channel_request;
        case handler_kind_t::event:
            return handler_dispatch_kind_t::classic_fanout;
        case handler_kind_t::send:
        case handler_kind_t::raw:
            return handler_dispatch_kind_t::channel_send;
    }
    return handler_dispatch_kind_t::channel_send;
}

bool is_request_dispatch (handler_dispatch_kind_t kind)
{
    return kind == handler_dispatch_kind_t::node_direct_request
           || kind == handler_dispatch_kind_t::channel_request;
}

struct filter_next_state_t
{
    std::mutex mutex;
    bool called = false;
    bool duplicate = false;
    std::optional<result_t<zlink::message_t>> downstream;
};

using filter_list_t = std::vector<handler_registry_t::filter_invoker_t>;
using filter_terminal_t = std::function<task_t<zlink::message_t> ()>;

task_t<zlink::message_t>
invoke_filter_level (const std::shared_ptr<const filter_list_t> &filters,
                     std::size_t index,
                     service_provider_t *services,
                     serializer_registry_t *serializers,
                     handler_filter_context_t context,
                     const std::shared_ptr<filter_terminal_t> &terminal);

task_t<void>
continue_filter_chain (const std::shared_ptr<filter_next_state_t> &state,
                       const std::shared_ptr<const filter_list_t> &filters,
                       std::size_t next_index,
                       service_provider_t *services,
                       serializer_registry_t *serializers,
                       handler_filter_context_t context,
                       const std::shared_ptr<filter_terminal_t> &terminal)
{
    {
        std::lock_guard lock (state->mutex);
        if (state->called) {
            state->duplicate = true;
            throw framework_exception_t (
              framework_error_kind_t::invalid_operation,
              "handler filter next may be invoked at most once");
        }
        state->called = true;
    }

    try {
        auto message =
          co_await invoke_filter_level (
            filters, next_index, services, serializers, std::move (context),
            terminal);
        std::lock_guard lock (state->mutex);
        state->downstream =
          result_t<zlink::message_t>::success (std::move (message));
    }
    catch (const framework_exception_t &error) {
        std::lock_guard lock (state->mutex);
        state->downstream =
          detail::result_access_t::failure<zlink::message_t> (error);
        throw;
    }
    co_return;
}

task_t<zlink::message_t>
invoke_filter_level (const std::shared_ptr<const filter_list_t> &filters,
                     std::size_t index,
                     service_provider_t *services,
                     serializer_registry_t *serializers,
                     handler_filter_context_t context,
    const std::shared_ptr<filter_terminal_t> &terminal)
{
    if (index >= filters->size ()) {
        co_return co_await (*terminal) ();
    }

    auto next_state = std::make_shared<filter_next_state_t> ();
    co_await (*filters)[index] (
      *services, *serializers, context,
      [next_state, filters, next_index = index + 1, services, serializers, context,
       terminal] () mutable {
          return continue_filter_chain (next_state, filters, next_index, services, serializers,
                                        std::move (context), terminal);
      });

    std::lock_guard lock (next_state->mutex);
    if (next_state->duplicate) {
        throw framework_exception_t (
          framework_error_kind_t::invalid_operation,
          "handler filter next may be invoked at most once");
    }
    if (!next_state->called) {
        if (is_request_dispatch (context.dispatch_kind)) {
            throw framework_exception_t (
              framework_error_kind_t::rejected,
              "handler filter rejected the request without invoking next");
        }
        co_return zlink::message_t{};
    }
    if (!next_state->downstream) {
        throw framework_exception_t (
          framework_error_kind_t::internal_failure,
          "handler filter returned before its continuation completed");
    }
    if (!*next_state->downstream) {
        throw *next_state->downstream->error ();
    }
    co_return next_state->downstream->value ();
}

} // namespace

namespace detail
{

struct handler_entry_t
{
    handler_descriptor_t descriptor;
    handler_registry_t::invoker_t invoker;
};

struct handler_key_t
{
    std::string channel_name;
    std::string topic;
    std::string packet_name;

    friend bool operator< (const handler_key_t &left, const handler_key_t &right) noexcept
    {
        if (left.channel_name != right.channel_name) {
            return left.channel_name < right.channel_name;
        }
        if (left.topic != right.topic) {
            return left.topic < right.topic;
        }
        return left.packet_name < right.packet_name;
    }
};

handler_key_t make_handler_key (std::string_view channel_name,
                                std::string_view topic,
                                std::string_view packet_name)
{
    return {std::string (channel_name), std::string (topic), std::string (packet_name)};
}

const handler_entry_t *
find_by_channel_packet (const std::map<handler_key_t, handler_entry_t> &handlers,
                        std::string_view channel_name,
                        std::string_view packet_name)
{
    for (const auto &[_, entry] : handlers) {
        if (entry.descriptor.channel_name == channel_name
            && entry.descriptor.packet_name == packet_name) {
            return &entry;
        }
    }
    return nullptr;
}

/// Fills the routing-derived fields the caller left open so every handler sees the same context
/// regardless of which dispatch surface produced it.
inbound_message_context_t resolve_inbound_context (const inbound_message_context_t &inbound,
                                                   const handler_descriptor_t &descriptor,
                                                   std::string_view channel_name,
                                                   std::string_view packet_name)
{
    inbound_message_context_t resolved = inbound;
    if (!resolved.message.channel_name && !channel_name.empty ()) {
        resolved.message.channel_name = std::string (channel_name);
    }
    if (resolved.message.packet_name.empty ()) {
        resolved.message.packet_name =
          packet_name.empty () ? descriptor.packet_name : std::string (packet_name);
    }
    if (resolved.topic.empty ()) {
        resolved.topic = descriptor.topic;
    }
    return resolved;
}

class handler_registry_state_t
{
  public:
    std::map<handler_key_t, handler_entry_t> handlers;
    std::vector<handler_registry_t::filter_invoker_t> filters;
    handler_registry_t::failure_observer_t failure_observer;
};

void configure_handler_invocation_executor ()
{
    ensure_handler_invocation_executor ();
}

void shutdown_handler_invocation_executor () noexcept
{
    std::shared_ptr<runtime::offload_executor_t> executor;
    {
        std::lock_guard lock (handler_invocation_executor_mutex ());
        executor = std::move (handler_invocation_executor_ref ());
    }
    if (executor) {
        executor->drain ();
    }
}

} // namespace detail

handler_registry_t::handler_registry_t () :
    _state (std::make_unique<detail::handler_registry_state_t> ())
{
}

handler_registry_t::~handler_registry_t () = default;

handler_registry_t::handler_registry_t (handler_registry_t &&) noexcept = default;

handler_registry_t &handler_registry_t::operator= (handler_registry_t &&) noexcept = default;

handler_registry_t &handler_registry_t::send_raw (std::string channel_name,
                                                  std::string packet_name,
                                                  raw_handler_t handler,
                                                  handler_options_t options)
{
    return send_raw (std::move (channel_name), "", std::move (packet_name), std::move (handler),
                     std::move (options));
}

handler_registry_t &handler_registry_t::send_raw (std::string channel_name,
                                                  std::string topic,
                                                  std::string packet_name,
                                                  raw_handler_t handler,
                                                  handler_options_t options)
{
    const auto packet = options.packet_name.value_or (packet_name);
    return add_handler (
      {std::move (channel_name), std::move (topic), packet, handler_kind_t::raw, options.execution,
       std::type_index (typeid (void)), std::type_index (typeid (zlink::message_t))},
      [handler = std::move (handler)] (
        service_provider_t &, serializer_registry_t &, const zlink::message_t &message,
        const detail::inbound_message_context_t &) -> task_t<zlink::message_t> {
          const auto result = handler (payload_view_t (detail::encoded_payload_from_raw (message)));
          if (!result) {
              return task_t<zlink::message_t> (detail::propagate_failure<zlink::message_t> (result, "raw handler failed"));
          }
          return task_t<zlink::message_t> (
            result_t<zlink::message_t>::success (zlink::message_t{}));
      });
}

handler_registry_t &handler_registry_t::observe_failures (failure_observer_t observer)
{
    _state->failure_observer = std::move (observer);
    return *this;
}

handler_registry_t &handler_registry_t::add_filter (filter_invoker_t filter)
{
    _state->filters.push_back (std::move (filter));
    return *this;
}

task_t<zlink::message_t>
handler_registry_t::invoke_filters_async (handler_dispatch_kind_t dispatch_kind,
                                          service_provider_t &services,
                                          serializer_registry_t &serializers,
                                          const message_context_t &context,
                                          terminal_invoker_t terminal) const
{
    auto filter_context = handler_filter_context_t{context, dispatch_kind};
    auto filters = std::make_shared<const filter_list_t> (_state->filters);
    auto owned_terminal =
      std::make_shared<filter_terminal_t> (std::move (terminal));
    return invoke_filter_level (filters, 0, &services, &serializers, std::move (filter_context),
                                owned_terminal);
}

const handler_descriptor_t *handler_registry_t::find (std::string_view channel_name,
                                                      std::string_view packet_name) const
{
    return find (channel_name, "", packet_name);
}

const handler_descriptor_t *handler_registry_t::find (std::string_view channel_name,
                                                      std::string_view topic,
                                                      std::string_view packet_name) const
{
    const auto found =
      _state->handlers.find (detail::make_handler_key (channel_name, topic, packet_name));
    if (found == _state->handlers.end ()) {
        return nullptr;
    }
    return &found->second.descriptor;
}

result_t<zlink::message_t> handler_registry_t::invoke (std::string_view channel_name,
                                                       std::string_view packet_name,
                                                       service_provider_t &services,
                                                       serializer_registry_t &serializers,
                                                       const zlink::message_t &message,
                                                       const detail::inbound_message_context_t
                                                         &inbound) const
{
    return invoke (channel_name, "", packet_name, services, serializers, message, inbound);
}

result_t<zlink::message_t> handler_registry_t::invoke (std::string_view channel_name,
                                                       std::string_view topic,
                                                       std::string_view packet_name,
                                                       service_provider_t &services,
                                                       serializer_registry_t &serializers,
                                                       const zlink::message_t &message,
                                                       const detail::inbound_message_context_t
                                                         &inbound) const
{
    const auto found =
      _state->handlers.find (detail::make_handler_key (channel_name, topic, packet_name));
    const detail::handler_entry_t *entry = nullptr;
    if (found != _state->handlers.end ()) {
        entry = &found->second;
    } else if (topic.empty ()) {
        entry = detail::find_by_channel_packet (_state->handlers, channel_name, packet_name);
    }
    if (entry == nullptr) {
        return result_t<zlink::message_t>::failure (framework_error_kind_t::not_found,
                                                    "handler is not registered");
    }

    auto invoke_body = [this, entry, &services, &serializers,
                        owned_message = std::make_shared<zlink::message_t> (message),
                        owned_inbound = detail::resolve_inbound_context (
                          inbound, entry->descriptor, channel_name, packet_name)] () mutable {
        result_t<zlink::message_t> result =
          result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                               "handler failed");
        try {
            result =
              invoke_filters_async (
                dispatch_kind_for (entry->descriptor.kind), services, serializers,
                owned_inbound.message,
                [&services, &serializers, entry, owned_message,
                 &owned_inbound] {
                    return entry->invoker (services, serializers, *owned_message,
                                           owned_inbound);
                })
                .result ();
        }
        catch (const framework_exception_t &error) {
            result = detail::result_access_t::failure<zlink::message_t> (error);
        }
        catch (...) {
            result = result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                          "handler threw an exception");
        }
        if (!result && result.error () != nullptr) {
            emit_failure (entry->descriptor, *result.error ());
        }
        return result;
    };

    detail::task_completion_source_t<zlink::message_t> completion;
    auto task = completion.task ();
    try {
        auto executor = handler_invocation_executor ();
        if (!executor) {
            return detail::boundary_failure<zlink::message_t> (detail::boundary_error_t::shutdown, "handler invocation executor is not running");
        }
        executor->submit (
          [completion = std::move (completion), invoke_body = std::move (invoke_body)] () mutable {
              completion.complete (invoke_body ());
          });
    }
    catch (const std::exception &error) {
        completion.complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        completion.complete (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "handler executor rejected invocation"));
    }
    return task.result ();
}

task_t<zlink::message_t> handler_registry_t::invoke_async (std::string_view channel_name,
                                                           std::string_view packet_name,
                                                           service_provider_t &services,
                                                           serializer_registry_t &serializers,
                                                           const zlink::message_t &message,
                                                           const detail::inbound_message_context_t
                                                             &inbound) const
{
    return invoke_async (channel_name, "", packet_name, services, serializers, message, inbound);
}

task_t<zlink::message_t> handler_registry_t::invoke_async (std::string_view channel_name,
                                                           std::string_view topic,
                                                           std::string_view packet_name,
                                                           service_provider_t &services,
                                                           serializer_registry_t &serializers,
                                                           const zlink::message_t &message,
                                                           const detail::inbound_message_context_t
                                                             &inbound) const
{
    const auto found =
      _state->handlers.find (detail::make_handler_key (channel_name, topic, packet_name));
    const detail::handler_entry_t *entry = nullptr;
    if (found != _state->handlers.end ()) {
        entry = &found->second;
    } else if (topic.empty ()) {
        entry = detail::find_by_channel_packet (_state->handlers, channel_name, packet_name);
    }
    if (entry == nullptr) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::not_found, "handler is not registered"));
    }
    auto owned_message = std::make_shared<zlink::message_t> (message);
    auto owned_inbound =
      detail::resolve_inbound_context (inbound, entry->descriptor, channel_name, packet_name);
    auto invoke_body = [this, entry, &services, &serializers,
                        owned_message = std::move (owned_message),
                        owned_inbound = std::move (
                          owned_inbound)] () mutable -> boost::asio::awaitable<
      result_t<zlink::message_t>> {
        result_t<zlink::message_t> result = result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "handler failed");
        try {
            result = co_await runtime::await_task_result (
              invoke_filters_async (
                dispatch_kind_for (entry->descriptor.kind), services, serializers,
                owned_inbound.message,
                [&services, &serializers, entry, owned_message,
                 &owned_inbound] {
                    return entry->invoker (services, serializers, *owned_message,
                                           owned_inbound);
                }));
        }
        catch (const framework_exception_t &error) {
            result = detail::result_access_t::failure<zlink::message_t> (error);
        }
        catch (...) {
            result = result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                          "handler threw an exception");
        }
        if (!result && result.error () != nullptr) {
            emit_failure (entry->descriptor, *result.error ());
        }
        co_return result;
    };

    try {
        return runtime::handler_coroutine_executor ().submit<zlink::message_t> (
          std::move (invoke_body));
    }
    catch (const std::exception &error) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "handler executor rejected invocation"));
    }
}

handler_registry_t &handler_registry_t::add_handler (handler_descriptor_t descriptor,
                                                     invoker_t invoker)
{
    const auto duplicate_packet =
      std::any_of (_state->handlers.begin (), _state->handlers.end (), [&] (const auto &entry) {
          const auto &existing = entry.second.descriptor;
          if (existing.channel_name != descriptor.channel_name || existing.kind != descriptor.kind
              || existing.packet_name != descriptor.packet_name) {
              return false;
          }
          return existing.topic == descriptor.topic || existing.topic.empty ()
                 || descriptor.topic.empty ();
      });
    if (duplicate_packet) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate handler registration");
    }
    const auto key =
      detail::make_handler_key (descriptor.channel_name, descriptor.topic, descriptor.packet_name);
    const auto [_, inserted] = _state->handlers.emplace (
      key, detail::handler_entry_t{std::move (descriptor), std::move (invoker)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate handler registration");
    }
    return *this;
}

void handler_registry_t::emit_failure (const handler_descriptor_t &descriptor,
                                       const framework_exception_t &error) const
{
    if (!_state->failure_observer) {
        return;
    }
    _state->failure_observer (
      handler_failure_event_t{descriptor, error.kind (), error.what ()});
}

} // namespace zlink::framework
