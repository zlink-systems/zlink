/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/server_support.hpp"

namespace zlink::framework::e2e::pubsub::server::publisher
{

struct publisher_options_t
{
    std::string log_dir;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string publisher_endpoint;
    std::string http_endpoint;

    static publisher_options_t bind (const configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .publisher_endpoint = section.require ("publisherEndpoint"),
                .http_endpoint = section.require ("httpEndpoint")};
    }
};

} // namespace zlink::framework::e2e::pubsub::server::publisher
