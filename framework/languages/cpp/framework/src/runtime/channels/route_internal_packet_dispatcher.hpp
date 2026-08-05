/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/route_packet.hpp"
#include "zlink/framework/contracts/configuration/services.hpp"

#include <memory>
#include <vector>

namespace zlink::framework::detail
{

class route_internal_packet_dispatcher_t
{
  public:
    virtual ~route_internal_packet_dispatcher_t () = default;

    virtual bool can_handle_send (std::string_view packet_name) const = 0;
    virtual bool can_handle_request (std::string_view packet_name) const = 0;

    virtual result_t<void> dispatch_send (const route_received_packet_t &received,
                                          service_provider_t &services) const = 0;

    virtual result_t<zlink::message_t>
    dispatch_request (const route_received_packet_t &received,
                      const runtime::messaging::envelope_header_t &header,
                      service_provider_t &services) const = 0;
};

class no_route_internal_packet_dispatcher_t final : public route_internal_packet_dispatcher_t
{
  public:
    bool can_handle_send (std::string_view packet_name) const override;
    bool can_handle_request (std::string_view packet_name) const override;
    result_t<void> dispatch_send (const route_received_packet_t &received,
                                  service_provider_t &services) const override;
    result_t<zlink::message_t>
    dispatch_request (const route_received_packet_t &received,
                      const runtime::messaging::envelope_header_t &header,
                      service_provider_t &services) const override;
};

class composite_route_internal_packet_dispatcher_t final : public route_internal_packet_dispatcher_t
{
  public:
    void add (const route_internal_packet_dispatcher_t &dispatcher);
    void add (std::shared_ptr<route_internal_packet_dispatcher_t> dispatcher);

    bool can_handle_send (std::string_view packet_name) const override;
    bool can_handle_request (std::string_view packet_name) const override;
    result_t<void> dispatch_send (const route_received_packet_t &received,
                                  service_provider_t &services) const override;
    result_t<zlink::message_t>
    dispatch_request (const route_received_packet_t &received,
                      const runtime::messaging::envelope_header_t &header,
                      service_provider_t &services) const override;

  private:
    const route_internal_packet_dispatcher_t *resolve_send (std::string_view packet_name) const;
    const route_internal_packet_dispatcher_t *resolve_request (std::string_view packet_name) const;

    std::vector<const route_internal_packet_dispatcher_t *> _dispatchers;
    std::vector<std::shared_ptr<route_internal_packet_dispatcher_t>> _owned_dispatchers;
};

} // namespace zlink::framework::detail
