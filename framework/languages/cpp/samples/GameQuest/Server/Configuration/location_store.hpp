/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <memory>
#include <stdexcept>

namespace zlink::samples::gamequest
{

inline void add_gamequest_location_store (zlink::framework::zlink_framework_options_t &framework,
                                          const sample_topology_t &topology)
{
    if (topology.redis_endpoint.empty ()) {
        throw std::runtime_error ("GAMEQUEST_REDIS_ENDPOINT is required");
    }
    if (topology.redis_key_prefix.empty ()) {
        throw std::runtime_error ("GAMEQUEST_REDIS_KEY_PREFIX is required");
    }
    framework.add_location_store (
      std::make_shared<zlink::framework::redis::redis_location_store_t> (
        zlink::framework::redis::redis_location_options_t{
          .connection_string = topology.redis_endpoint,
          .key_prefix = topology.redis_key_prefix}));
    framework.add_relocation_store (
      std::make_shared<zlink::framework::redis::redis_relocation_store_t> (
        zlink::framework::redis::redis_relocation_options_t{
          .connection_string = topology.redis_endpoint,
          .key_prefix = topology.redis_key_prefix + "relocation:"}));
}

} // namespace zlink::samples::gamequest
