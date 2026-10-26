/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"

namespace zlink::framework::detail
{

task_t<void>
route_handler_invoker_t::invoke_send (const route_handler_registry_t &handlers,
                                      const handler_registry_t &filters,
                                      handler_dispatch_kind_t dispatch_kind,
                                      service_provider_t &services,
                                      serializer_registry_t &serializers,
                                      zlink::message_t message,
                                      framework::route_message_context_t context,
                                      std::function<void ()> before_application_handler)
{
    auto invocation_scope =
      service_scope_t::create (services, service_scope_kind_t::handler_invocation);
    auto &invocation_services = invocation_scope.provider ();
    co_await filters.invoke_filters_async (
      dispatch_kind, invocation_services, serializers, context,
      [&handlers, &invocation_services, &serializers, &message,
       &context, before_application_handler = std::move (before_application_handler)] () mutable {
          if (before_application_handler) {
              before_application_handler ();
          }
          return handlers.invoke_async (*context.mesh_name,
                                        runtime::messaging::message_kind_t::command, context.packet_name,
                                        invocation_services, serializers, message, context);
      });
    co_return;
}

task_t<zlink::message_t>
route_handler_invoker_t::invoke_request (const route_handler_registry_t &handlers,
                                         const handler_registry_t &filters,
                                         handler_dispatch_kind_t dispatch_kind,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         zlink::message_t message,
                                         framework::route_message_context_t context,
                                         std::function<void ()> before_application_handler)
{
    try {
        auto invocation_scope =
          service_scope_t::create (services, service_scope_kind_t::handler_invocation);
        auto &invocation_services = invocation_scope.provider ();
        co_return co_await filters.invoke_filters_async (
          dispatch_kind, invocation_services, serializers, context,
          [&handlers, &invocation_services, &serializers, &message,
           &context,
           before_application_handler = std::move (before_application_handler)] () mutable {
              if (before_application_handler) {
                  before_application_handler ();
              }
              return handlers.invoke_async (
                *context.mesh_name, runtime::messaging::message_kind_t::request, context.packet_name,
                invocation_services, serializers, message, context);
          });
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<zlink::message_t> (error);
    }
    catch (...) {
        co_return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                       "routed request handler threw an exception");
    }
}

} // namespace zlink::framework::detail
