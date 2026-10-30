/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/mesh/application_dispatch_terminal_owner.hpp"

#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/route_packet.hpp"
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

namespace zlink::framework::detail
{
class route_handler_invoker_t
{
  public:
    using completion_t = std::function<result_t<std::optional<route_dispatch_reply_t>> (
      const result_t<zlink::message_t> &)>;

    static task_t<std::optional<route_dispatch_reply_t>>
    invoke (const route_handler_registry_t::entry_t &handler,
            const handler_registry_t &filters,
            handler_dispatch_kind_t dispatch_kind,
            service_provider_t &services,
            serializer_registry_t &serializers,
            zlink::message_t message,
            framework::route_message_context_t context,
            std::function<void ()> before_application_handler = {},
            completion_t complete = {},
            runtime::application_dispatch_terminal_owner_t terminal = {},
            runtime::offload_executor_t *executor = nullptr);
};
} // namespace zlink::framework::detail
