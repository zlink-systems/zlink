/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <memory>
#include <stdexcept>

namespace zlink::samples::bingo
{

inline void add_sample_location_store (framework::zlink_framework_options_t &options,
                                       const sample_topology_t &topology)
{
    if (topology.redis_endpoint.empty ()) {
        throw std::runtime_error ("Bingo sample requires redisEndpoint");
    }
    if (topology.redis_key_prefix.empty ()) {
        throw std::runtime_error ("Bingo sample requires redisKeyPrefix");
    }
    options.add_location_store (
      std::make_shared<framework::redis::redis_location_store_t> (
        framework::redis::redis_location_options_t{
          .connection_string = topology.redis_endpoint,
          .key_prefix = topology.redis_key_prefix + "location:"}));
    options.add_relocation_store (
      std::make_shared<framework::redis::redis_relocation_store_t> (
        framework::redis::redis_relocation_options_t{
          .connection_string = topology.redis_endpoint,
          .key_prefix = topology.redis_key_prefix + "relocation:"}));
}

} // namespace zlink::samples::bingo
