/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/channel_bundle_factory.hpp"

#include <utility>

namespace zlink::framework::detail
{

std::shared_ptr<channel_runtime_bundle_t>
channel_bundle_factory_t::create_client_bundle (const std::string &channel_name,
                                                const channel_snapshot_t &channel) const
{
    return create_bundle (channel_name, channel.client, true);
}

std::shared_ptr<channel_runtime_bundle_t>
channel_bundle_factory_t::create_server_bundle (const std::string &channel_name,
                                                const channel_snapshot_t &channel) const
{
    return create_bundle (channel_name, channel.server, true);
}

std::shared_ptr<channel_runtime_bundle_t>
channel_bundle_factory_t::create_publisher_bundle (const std::string &channel_name,
                                                   const channel_snapshot_t &channel) const
{
    return create_bundle (channel_name, channel.publisher, true);
}

std::shared_ptr<channel_runtime_bundle_t>
channel_bundle_factory_t::create_subscriber_bundle (const std::string &channel_name,
                                                    const channel_snapshot_t &channel) const
{
    return create_bundle (channel_name, channel.subscriber, true);
}

std::shared_ptr<channel_runtime_bundle_t>
channel_bundle_factory_t::create_bundle (const std::string &channel_name,
                                         const channel_capability_snapshot_t &capability,
                                         bool attach_bind_endpoints)
{
    if (!capability.enabled) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "channel capability '" + channel_name + "' is not enabled");
    }

    auto bundle = std::make_shared<channel_runtime_bundle_t> ();
    for (const auto &endpoint : capability.connect_endpoints) {
        bundle->try_add_manual_connection (endpoint);
    }
    if (attach_bind_endpoints) {
        for (const auto &endpoint : capability.bind_endpoints) {
            bundle->try_add_manual_connection (endpoint);
        }
    }
    return bundle;
}

} // namespace zlink::framework::detail
