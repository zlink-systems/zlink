/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/locations/redis.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::framework::test
{
inline std::string unique_redis_prefix ()
{
    return "zlink:cpp:opaque-redis-test:"
           + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ());
}

// ZLINK_REDIS_TEST_ENDPOINT (host:port) names the one Redis a test run uses in
// every language (#4542). When it is set an unreachable Redis fails the test;
// without it the tests try 127.0.0.1:6379 and skip when nothing answers.
inline std::optional<redis::redis_location_options_t> find_redis_options ()
{
#if defined(ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT)
    const auto *provided = std::getenv ("ZLINK_REDIS_TEST_ENDPOINT");
    const std::string endpoint =
      provided != nullptr && *provided != '\0' ? provided : "127.0.0.1:6379";
    redis::redis_location_options_t options{.connection_string = "tcp://" + endpoint,
                                            .key_prefix = unique_redis_prefix ()};
    redis::redis_location_store_t store (options);
    location_store_t &provider = store;
    if (provider.read ({.value = "probe"}).result ().has_value ())
        return options;
    if (provided != nullptr && *provided != '\0')
        throw std::runtime_error ("ZLINK_REDIS_TEST_ENDPOINT is set but Redis is unreachable at "
                                  + endpoint);
#endif
    return std::nullopt;
}
}
