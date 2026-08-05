/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"

namespace zlink::framework::detail
{

class channel_packet_dispatcher_t
{
  public:
    explicit channel_packet_dispatcher_t (channel_runtime_t runtime);

    result_t<runtime::messaging::message_parts_t>
    dispatch_server_message (std::string channel_name,
                             const runtime::messaging::message_parts_t &parts,
                             service_provider_t &services,
                             serializer_registry_t &serializers,
                             const handler_registry_t &handlers) const;

  private:
    channel_runtime_t _runtime;
};

} // namespace zlink::framework::detail
