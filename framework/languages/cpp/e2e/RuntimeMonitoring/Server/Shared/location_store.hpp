/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::server
{

inline void add_redis_location_store (zlink::framework::zlink_framework_options_t &framework,
                                      const std::string &redis_endpoint,
                                      const std::string &redis_key_prefix)
{
    if (redis_endpoint.empty ()) {
        throw std::runtime_error ("ZLINK_CPP_E2E_REDIS_ENDPOINT is required");
    }
    if (redis_key_prefix.empty ()) {
        throw std::runtime_error ("ZLINK_CPP_E2E_REDIS_KEY_PREFIX is required");
    }
    framework.add_location_store (
      std::make_shared<zlink::framework::redis::redis_location_store_t> (
        zlink::framework::redis::redis_location_options_t{
          .connection_string = redis_endpoint, .key_prefix = redis_key_prefix}));
    auto &locations = framework.configure_locations ();
    locations.owner_lease_renew_interval = std::chrono::seconds (1);
    locations.owner_lease_renew_timeout = std::chrono::milliseconds (500);
    locations.owner_lease_ttl = std::chrono::seconds (5);
    locations.owner_lease_fencing_margin = std::chrono::seconds (1);
    locations.polling_interval = std::chrono::milliseconds (500);
}

} // namespace zlink::framework::e2e::runtime_monitoring::server
