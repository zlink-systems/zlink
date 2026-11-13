/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/dispatch_options_access.hpp"

#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/channels/route_packet.hpp"

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <functional>
#include <optional>
#include <string>
#include <memory>

namespace zlink::framework::detail
{

constexpr bool is_node_or_channel_record_kind (runtime::host::record_kind_t kind) noexcept
{
    using record_kind_t = runtime::host::record_kind_t;
    return kind == record_kind_t::node_send || kind == record_kind_t::node_request
           || kind == record_kind_t::channel_send || kind == record_kind_t::channel_request;
}

class route_packet_dispatcher_t
{
  public:
    explicit route_packet_dispatcher_t (std::string router_channel_id);
    route_packet_dispatcher_t (
      std::string router_channel_id,
      service_provider_t &services,
      serializer_registry_t &serializers,
      const route_handler_registry_t &handlers,
      const route_internal_packet_dispatcher_t &internal_packets,
      retained_dispatch_options_t dispatch_options = {},
      const handler_registry_t *filters = nullptr,
      handler_dispatch_kind_t send_dispatch_kind = handler_dispatch_kind_t::node_direct_send,
      handler_dispatch_kind_t request_dispatch_kind = handler_dispatch_kind_t::node_direct_request,
      std::function<void ()> before_application_handler = {},
      std::shared_ptr<void> registration = {},
      std::function<result_t<void> (route_dispatch_reply_t &&)> submit_reply = {});

    route_packet_dispatcher_t (service_provider_t &services,
                               serializer_registry_t &serializers,
                               const route_handler_registry_t &handlers,
                               const handler_registry_t &filters,
                               retained_dispatch_options_t options = {},
                               std::function<void ()> before = {},
                               std::shared_ptr<void> registration = {});

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const runtime::host::receive_record_t &record,
              std::vector<zlink::message_t> parts,
              runtime::messaging::envelope_header_view_t header,
              runtime::application_dispatch_terminal_owner_t terminal = {},
              runtime::offload_executor_t *executor = nullptr,
              const route_handler_registry_t::prepared_header_t *prepared = nullptr) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const route_received_packet_t &received) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const route_received_packet_t &received,
              runtime::messaging::envelope_header_view_t incoming_header,
              runtime::application_dispatch_terminal_owner_t terminal = {},
              runtime::offload_executor_t *executor = nullptr,
              const route_handler_registry_t::prepared_header_t *prepared = nullptr) const;

  private:
    struct binding_t
    {
        const std::string *channel_override;
        std::string_view
        channel (const runtime::messaging::envelope_header_t &header) const noexcept
        {
            return channel_override ? std::string_view (*channel_override)
                                    : std::string_view (header.channel_name);
        }
        handler_dispatch_kind_t send_kind;
        handler_dispatch_kind_t request_kind;
        const route_internal_packet_dispatcher_t *internal;
        route_reply_target_t reply;
    };

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch (const route_received_packet_t &received,
              runtime::messaging::envelope_header_view_t header,
              runtime::application_dispatch_terminal_owner_t terminal,
              runtime::offload_executor_t *executor,
              const route_handler_registry_t::prepared_header_t *prepared,
              const binding_t &binding) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch_send (const route_received_packet_t &received,
                   runtime::messaging::envelope_header_view_t incoming_header,
                   runtime::application_dispatch_terminal_owner_t terminal,
                   runtime::offload_executor_t *executor,
                   const route_handler_registry_t::entry_t *handler,
                   const binding_t &binding) const;

    task_t<std::optional<route_dispatch_reply_t>>
    dispatch_request (const route_received_packet_t &received,
                      runtime::messaging::envelope_header_view_t incoming_header,
                      runtime::application_dispatch_terminal_owner_t terminal,
                      runtime::offload_executor_t *executor,
                      const route_handler_registry_t::entry_t *handler,
                      const binding_t &binding) const;

    std::string _router_channel_id;
    service_provider_t *_services = nullptr;
    serializer_registry_t *_serializers = nullptr;
    const route_handler_registry_t *_handlers = nullptr;
    const handler_registry_t *_filters = nullptr;
    const route_internal_packet_dispatcher_t *_internal_packets = nullptr;
    retained_dispatch_options_t _dispatch_options;
    handler_dispatch_kind_t _send_dispatch_kind = handler_dispatch_kind_t::node_direct_send;
    handler_dispatch_kind_t _request_dispatch_kind = handler_dispatch_kind_t::node_direct_request;
    std::function<void ()> _before_application_handler;
    std::shared_ptr<void> _registration;
    std::function<result_t<void> (route_dispatch_reply_t &&)> _submit_reply;
};

} // namespace zlink::framework::detail
