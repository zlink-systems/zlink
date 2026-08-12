/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "configuration.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

namespace zlink::samples::zoneworld
{

inline void add_stores (framework::zlink_framework_options_t &options,
                        const configuration_t &configuration)
{
    options.add_location_store (
      std::make_shared<framework::redis::redis_location_store_t> (
        framework::redis::redis_location_options_t{
          .connection_string = configuration.redis_endpoint,
          .key_prefix = configuration.redis_key_prefix + "location:"}));
    options.add_relocation_store (
      std::make_shared<framework::redis::redis_relocation_store_t> (
        framework::redis::redis_relocation_options_t{
          .connection_string = configuration.redis_endpoint,
          .key_prefix = configuration.redis_key_prefix + "relocation:"}));
}

} // namespace zlink::samples::zoneworld
