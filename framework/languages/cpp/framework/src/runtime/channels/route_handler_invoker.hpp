/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/route_handler_registry.hpp"

#include <zlink/framework/contracts/handlers/handler_registry.hpp>

namespace zlink::framework::detail
{

class route_handler_invoker_t
{
  public:
    task_t<void> invoke_send (const route_handler_registry_t &handlers,
                              const handler_registry_t &filters,
                              handler_dispatch_kind_t dispatch_kind,
                              std::string_view router_channel_id,
                              std::string_view packet_name,
                              service_provider_t &services,
                              serializer_registry_t &serializers,
                              const zlink::message_t &message,
                              const framework::route_message_context_t &context) const;

    task_t<zlink::message_t>
    invoke_request (const route_handler_registry_t &handlers,
                    const handler_registry_t &filters,
                    handler_dispatch_kind_t dispatch_kind,
                    std::string_view router_channel_id,
                    std::string_view packet_name,
                    service_provider_t &services,
                    serializer_registry_t &serializers,
                    const zlink::message_t &message,
                    const framework::route_message_context_t &context) const;
};

} // namespace zlink::framework::detail
