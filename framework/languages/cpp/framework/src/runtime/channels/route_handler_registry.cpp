/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_handler_registry.hpp"

#include <utility>

namespace zlink::framework::detail
{

route_handler_registry_t &
route_handler_registry_t::add_handler (route_handler_descriptor_t descriptor, invoker_t invoker)
{
    key_t key{descriptor.router_channel_id, descriptor.kind, descriptor.packet_name};
    const auto [_, inserted] =
      _handlers.emplace (std::move (key), entry_t{std::move (descriptor), std::move (invoker)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate routed handler registration");
    }
    return *this;
}

const route_handler_descriptor_t *
route_handler_registry_t::find (std::string_view router_channel_id,
                                runtime::messaging::message_kind_t kind,
                                std::string_view packet_name) const
{
    const auto found =
      _handlers.find (key_t{std::string (router_channel_id), kind, std::string (packet_name)});
    if (found == _handlers.end ()) {
        return nullptr;
    }
    return &found->second.descriptor;
}

result_t<zlink::message_t>
route_handler_registry_t::invoke (std::string_view router_channel_id,
                                  runtime::messaging::message_kind_t kind,
                                  std::string_view packet_name,
                                  service_provider_t &services,
                                  serializer_registry_t &serializers,
                                  const zlink::message_t &message,
                                  const framework::route_message_context_t &context) const
{
    return invoke_async (router_channel_id, kind, packet_name, services, serializers, message,
                         context)
      .result ();
}

task_t<zlink::message_t>
route_handler_registry_t::invoke_async (std::string_view router_channel_id,
                                        runtime::messaging::message_kind_t kind,
                                        std::string_view packet_name,
                                        service_provider_t &services,
                                        serializer_registry_t &serializers,
                                        const zlink::message_t &message,
                                        const framework::route_message_context_t &context) const
{
    const auto found =
      _handlers.find (key_t{std::string (router_channel_id), kind, std::string (packet_name)});
    if (found == _handlers.end ()) {
        return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
          framework_error_kind_t::not_found, "routed handler is not registered"));
    }
    return found->second.invoker (services, serializers, message, context);
}

} // namespace zlink::framework::detail
