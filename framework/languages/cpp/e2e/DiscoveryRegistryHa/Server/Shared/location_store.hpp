/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::store_failure::server
{

template<typename TOptions>
inline void add_redis_location_store (zlink::framework::zlink_framework_options_t &framework,
                                      const TOptions &options)
{
    if (options.redis_endpoint.empty ()) {
        throw std::runtime_error ("Redis endpoint is required");
    }
    if (options.redis_key_prefix.empty ()) {
        throw std::runtime_error ("Redis key prefix is required");
    }
    auto redis_store = std::make_shared<zlink::framework::redis::redis_location_store_t> (
      zlink::framework::redis::redis_location_options_t{
        .connection_string = options.redis_endpoint, .key_prefix = options.redis_key_prefix});
    framework.add_location_store (std::move (redis_store));
    auto &locations = framework.configure_locations ();
    locations.owner_lease_renew_interval =
      std::chrono::milliseconds (options.heartbeat_ms);
    locations.owner_lease_ttl = std::chrono::milliseconds (options.lease_ttl_ms);
    locations.owner_lease_fencing_margin = std::chrono::milliseconds (1000);
    locations.owner_lease_renew_timeout = std::chrono::milliseconds (500);
    locations.polling_interval = std::chrono::milliseconds (options.polling_ms);
    locations.store_failure_grace = std::chrono::milliseconds (options.grace_ms);
}

} // namespace zlink::framework::e2e::store_failure::server
