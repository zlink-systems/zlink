/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/configuration/service_scope.hpp"

namespace zlink::framework::detail
{

task_t<void> route_handler_invoker_t::invoke_send (
  const route_handler_registry_t::entry_t &handler,
  const handler_registry_t &filters,
  handler_dispatch_kind_t dispatch_kind,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const zlink::message_t &message,
  const framework::route_message_context_t &context,
  detail::handler_start_callback_ref_t before_application_handler) const
{
    auto invocation_scope =
      service_scope_t::create (services, service_scope_kind_t::handler_invocation);
    auto &invocation_services = invocation_scope.provider ();
    co_await filters.invoke_filters_async (dispatch_kind, invocation_services, serializers, context,
                                           [&handler, &invocation_services, &serializers, &message,
                                            &context, before_application_handler] {
                                               return handler.invoker (
                                                 invocation_services, serializers, message, context,
                                                 before_application_handler);
                                           });
    co_return;
}

task_t<zlink::message_t> route_handler_invoker_t::invoke_request (
  const route_handler_registry_t::entry_t &handler,
  const handler_registry_t &filters,
  handler_dispatch_kind_t dispatch_kind,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const zlink::message_t &message,
  const framework::route_message_context_t &context,
  detail::handler_start_callback_ref_t before_application_handler) const
{
    try {
        auto invocation_scope =
          service_scope_t::create (services, service_scope_kind_t::handler_invocation);
        auto &invocation_services = invocation_scope.provider ();
        co_return co_await filters.invoke_filters_async (
          dispatch_kind, invocation_services, serializers, context,
          [&handler, &invocation_services, &serializers, &message, &context,
           before_application_handler] {
              return handler.invoker (invocation_services, serializers, message, context,
                                      before_application_handler);
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
