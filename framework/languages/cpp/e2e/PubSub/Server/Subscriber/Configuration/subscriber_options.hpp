/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/server_support.hpp"

namespace zlink::framework::e2e::pubsub::server::subscriber
{

struct subscriber_options_t
{
    std::string log_dir;
    std::string subscriber_id;
    std::string topics;
    std::string accepted_topics;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string http_endpoint;
    int handler_delay_ms;

    static subscriber_options_t bind (const configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .subscriber_id = section.require ("subscriberId"),
                .topics = section.require ("topics"),
                .accepted_topics = section.require ("acceptedTopics"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .http_endpoint = section.require ("httpEndpoint"),
                .handler_delay_ms = std::stoi (section.get ("handlerDelayMs").value_or ("0"))};
    }
};

} // namespace zlink::framework::e2e::pubsub::server::subscriber
