/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/channels/channel_bundle_factory.hpp"

#include <memory>
#include <string>

namespace zlink::framework::detail
{

class channel_runtime_manager_t
{
  public:
    enum class capability_t
    {
        server,
        client,
        publisher,
        subscriber
    };

    explicit channel_runtime_manager_t (std::shared_ptr<channel_runtime_state_t> state);
    static channel_runtime_manager_t from (const message_bus_t &bus);
    static channel_runtime_manager_t from (const zlink_builder_t &builder);

    channel_runtime_bundle_t &get_or_create_client_bundle (const std::string &channel_name);
    channel_runtime_bundle_t &get_or_create_publisher_bundle (const std::string &channel_name);
    channel_runtime_bundle_t &get_or_create_subscriber_bundle (const std::string &channel_name);
    route_channel_runtime_t &get_route_channel (const std::string &router_channel_id);
    const route_handler_registry_t &get_route_handlers (const std::string &router_channel_id) const;
    std::vector<std::string> route_channel_ids () const;
    static std::vector<std::string> configured_route_channel_ids (const zlink_builder_t &builder);

    void initialize_inbound_channels ();
    void initialize_publisher_channels ();
    void initialize_client_channels ();
    void initialize_route_channels (const std::vector<std::string> &route_ids);
    void initialize_route_channels (
      const std::map<std::string, std::shared_ptr<route_channel_builder_state_t>> &registrations);
    void initialize_route_channels (const zlink_builder_t &builder);

    std::string monitoring_source (const std::string &source_name);

  private:
    const channel_snapshot_t &require_channel (const std::string &channel_name,
                                               capability_t capability) const;

    static std::pair<std::string, std::string> parse_source (const std::string &source_name);

    std::shared_ptr<channel_runtime_state_t> _state;
    channel_bundle_factory_t _bundle_factory;
};

} // namespace zlink::framework::detail
