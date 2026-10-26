/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/route_packet.hpp"
#include "runtime/channels/route_internal_packet_dispatcher.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

#include <functional>
#include <vector>
#include <variant>

namespace zlink::framework::detail
{

class mesh_record_dispatcher_t
{
  public:
    mesh_record_dispatcher_t (service_provider_t &services,
                              serializer_registry_t &serializers,
                              const route_handler_registry_t &handlers,
                              const handler_registry_t &filters,
                              dispatch_options_t dispatch_options = {},
                              std::function<void ()> before_application_handler = {});

    using dispatch_result_t = std::variant<result_t<void>, task_t<result_t<void>>>;
    dispatch_result_t dispatch (const runtime::host::receive_record_t &record,
                             std::vector<zlink::message_t> parts,
                             runtime::messaging::envelope_header_t header) const;

  private:
    static result_t<void> complete (
      const runtime::host::receive_record_t &record,
      const result_t<std::optional<route_dispatch_reply_t>> &dispatched);
    static task_t<result_t<void>> continue_dispatch (
      runtime::host::receive_record_t record,
      task_t<result_t<std::optional<route_dispatch_reply_t>>> pending,
      dispatch_options_t dispatch_options);

    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const route_handler_registry_t *_handlers;
    const handler_registry_t *_filters;
    dispatch_options_t _dispatch_options;
    no_route_internal_packet_dispatcher_t _no_internal_packets;
    std::function<void ()> _before_application_handler;
};

} // namespace zlink::framework::detail
