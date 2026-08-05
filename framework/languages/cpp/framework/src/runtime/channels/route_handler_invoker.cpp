/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"

#include "runtime/dispatch/coroutine_executor.hpp"

namespace zlink::framework::detail
{

task_t<void>
route_handler_invoker_t::invoke_send (const route_handler_registry_t &handlers,
                                      const handler_registry_t &filters,
                                      handler_dispatch_kind_t dispatch_kind,
                                      std::string_view router_channel_id,
                                      std::string_view packet_name,
                                      service_provider_t &services,
                                      serializer_registry_t &serializers,
                                      const zlink::message_t &message,
                                      const framework::route_message_context_t &context) const
{
    return runtime::handler_coroutine_executor ().submit<void> (
      [&handlers, &filters, dispatch_kind, router_channel_id = std::string (router_channel_id),
       packet_name = std::string (packet_name), &services, &serializers, message,
       context] () mutable -> boost::asio::awaitable<result_t<void>> {
          try {
              auto invocation_scope = service_scope_t::create (
                services, service_scope_kind_t::handler_invocation);
              auto &invocation_services = invocation_scope.provider ();
              auto result = co_await runtime::await_task_result (
                filters.invoke_filters_async (
                  dispatch_kind, invocation_services, serializers, context,
                  [&handlers, router_channel_id, packet_name, &invocation_services,
                   &serializers, message, context] {
                      return handlers.invoke_async (
                        router_channel_id, runtime::messaging::message_kind_t::command,
                        packet_name, invocation_services, serializers, message, context);
                  }));
              if (!result) {
                  co_return detail::propagate_failure<void> (result, "routed send handler failed");
              }
              co_return result_t<void>::success ();
          }
          catch (const framework_exception_t &error) {
              co_return detail::result_access_t::failure<void> (error);
          }
          catch (...) {
              co_return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                "routed send handler threw an exception");
          }
      });
}

task_t<zlink::message_t>
route_handler_invoker_t::invoke_request (const route_handler_registry_t &handlers,
                                         const handler_registry_t &filters,
                                         handler_dispatch_kind_t dispatch_kind,
                                         std::string_view router_channel_id,
                                         std::string_view packet_name,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         const zlink::message_t &message,
                                         const framework::route_message_context_t &context) const
{
    return runtime::handler_coroutine_executor ().submit<zlink::message_t> (
      [&handlers, &filters, dispatch_kind, router_channel_id = std::string (router_channel_id),
       packet_name = std::string (packet_name), &services, &serializers, message,
       context] () mutable -> boost::asio::awaitable<result_t<zlink::message_t>> {
          try {
              auto invocation_scope = service_scope_t::create (
                services, service_scope_kind_t::handler_invocation);
              auto &invocation_services = invocation_scope.provider ();
              co_return co_await runtime::await_task_result (
                filters.invoke_filters_async (
                  dispatch_kind, invocation_services, serializers, context,
                  [&handlers, router_channel_id, packet_name, &invocation_services,
                   &serializers, message, context] {
                      return handlers.invoke_async (
                        router_channel_id, runtime::messaging::message_kind_t::request,
                        packet_name, invocation_services, serializers, message, context);
                  }));
          }
          catch (const framework_exception_t &error) {
              co_return detail::result_access_t::failure<zlink::message_t> (error);
          }
          catch (...) {
              co_return result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, "routed request handler threw an exception");
          }
      });
}

} // namespace zlink::framework::detail
