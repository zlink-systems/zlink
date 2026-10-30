/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <functional>
#include <optional>

namespace zlink::framework::detail
{

class channel_packet_dispatcher_t
{
  public:
    explicit channel_packet_dispatcher_t (channel_runtime_t runtime);

    result_t<runtime::messaging::message_parts_t>
    dispatch_server_message (std::string channel_name,
                             runtime::messaging::message_parts_t parts,
                             service_provider_t &services,
                             serializer_registry_t &serializers,
                             const handler_registry_t &handlers,
                             std::function<void ()> before_application_handler = {}) const;

    task_t<runtime::messaging::message_parts_t> dispatch_server_message_in_handler (
      std::string channel_name,
      runtime::messaging::message_parts_t &&parts,
      service_provider_t &services,
      serializer_registry_t &serializers,
      const handler_registry_t &handlers,
      std::function<void ()> before_application_handler = {},
      std::optional<service_scope_t> scope = {},
      runtime::offload_executor_t *executor = nullptr,
      std::function<void (const result_t<runtime::messaging::message_parts_t> &,
                          const runtime::messaging::message_parts_t &)> complete = {}) const;

  private:
    channel_runtime_t _runtime;
};

} // namespace zlink::framework::detail
