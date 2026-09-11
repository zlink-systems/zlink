/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"

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
                                      const framework::route_message_context_t &context,
                                      std::function<void ()>
                                        before_application_handler) const
{
    auto invocation_scope = service_scope_t::create (
      services, service_scope_kind_t::handler_invocation);
    auto &invocation_services = invocation_scope.provider ();
    co_await filters.invoke_filters_async (
      dispatch_kind, invocation_services, serializers, context,
      [&handlers, router_channel_id = std::string (router_channel_id),
       packet_name = std::string (packet_name), &invocation_services,
       &serializers, message = message.copy (), context,
       before_application_handler = std::move (before_application_handler)] () mutable {
          if (before_application_handler) {
              before_application_handler ();
          }
          return handlers.invoke_async (
            router_channel_id, runtime::messaging::message_kind_t::command,
            packet_name, invocation_services, serializers, message, context);
      });
    co_return;
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
                                         const framework::route_message_context_t &context,
                                         std::function<void ()>
                                           before_application_handler) const
{
    try {
        auto invocation_scope = service_scope_t::create (
          services, service_scope_kind_t::handler_invocation);
        auto &invocation_services = invocation_scope.provider ();
        co_return co_await filters.invoke_filters_async (
          dispatch_kind, invocation_services, serializers, context,
          [&handlers, router_channel_id = std::string (router_channel_id),
           packet_name = std::string (packet_name), &invocation_services,
           &serializers, message = message.copy (), context,
           before_application_handler = std::move (before_application_handler)] () mutable {
              if (before_application_handler) {
                  before_application_handler ();
              }
              return handlers.invoke_async (
                router_channel_id, runtime::messaging::message_kind_t::request,
                packet_name, invocation_services, serializers, message, context);
          });
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (...) {
        co_return result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "routed request handler threw an exception");
    }
}

} // namespace zlink::framework::detail
