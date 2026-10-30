/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"

namespace zlink::framework::detail
{
task_t<std::optional<route_dispatch_reply_t>>
route_handler_invoker_t::invoke (const route_handler_registry_t::entry_t &handler,
                                 const handler_registry_t &filters,
                                 handler_dispatch_kind_t dispatch_kind,
                                 service_provider_t &services,
                                 serializer_registry_t &serializers,
                                 zlink::message_t message,
                                 framework::route_message_context_t context,
                                 std::function<void ()> before_application_handler,
                                 completion_t complete)
{
    std::optional<service_scope_t> invocation_scope;
    auto result = result_t<zlink::message_t>::success (zlink::message_t{});
    try {
        invocation_scope.emplace (
          service_scope_t::create (services, service_scope_kind_t::handler_invocation));
        auto &invocation_services = invocation_scope->provider ();
        auto reply = co_await filters.invoke_filters_async (
          dispatch_kind, invocation_services, serializers, context,
          [&handler, &invocation_services, &serializers, &message, &context,
           &before_application_handler] {
              return handler.invoker (invocation_services, serializers, message, context,
                                      handler_start_callback_ref_t (before_application_handler));
          });
        result = result_t<zlink::message_t>::success (std::move (reply));
    }
    catch (const framework_exception_t &error) {
        result = result_access_t::failure<zlink::message_t> (error);
    }
    catch (...) {
        result =
          dispatch_kind == handler_dispatch_kind_t::node_direct_request
              || dispatch_kind == handler_dispatch_kind_t::channel_request
            ? result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                   "routed request handler threw an exception")
            : current_exception_result<zlink::message_t> ();
    }
    // Reply encoding/submission and scope cleanup belong to this completing turn.
    // Physical terminal observers may release the App's executor join afterwards.
    auto completed = result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt);
    try {
        completed = complete ? complete (result)
                    : result
                      ? result_t<std::optional<route_dispatch_reply_t>>::success (std::nullopt)
                      : propagate_failure<std::optional<route_dispatch_reply_t>> (
                          result, "routed handler failed");
    }
    catch (...) {
        completed = current_exception_result<std::optional<route_dispatch_reply_t>> ();
    }
    if (invocation_scope)
        invocation_scope->close ();
    invocation_scope.reset ();
    before_application_handler = {};
    complete = {};
    message = zlink::message_t{};
    result = result_t<zlink::message_t>::success (zlink::message_t{});
    co_return completed;
}
} // namespace zlink::framework::detail
