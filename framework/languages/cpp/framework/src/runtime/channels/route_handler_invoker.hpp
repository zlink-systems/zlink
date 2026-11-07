/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/application_dispatch_terminal_owner.hpp"

#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/route_packet.hpp"
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

namespace zlink::framework::detail
{
struct route_request_completion_t
{
    retained_dispatch_options_t options;
    std::shared_ptr<void> registration;
    std::function<result_t<void> (route_dispatch_reply_t &&)> submit_reply;
    route_received_packet_t received;
    runtime::messaging::envelope_header_t header;
    result_t<std::optional<route_dispatch_reply_t>>
    operator() (const result_t<zlink::message_t> &reply,
                const framework::route_message_context_t &context) const;
};

class route_handler_invoker_t
{
  public:
    using callback_completion_t = std::function<result_t<std::optional<route_dispatch_reply_t>> (
      const result_t<zlink::message_t> &)>;

    using completion_t =
      std::variant<std::monostate, callback_completion_t, route_request_completion_t>;

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
