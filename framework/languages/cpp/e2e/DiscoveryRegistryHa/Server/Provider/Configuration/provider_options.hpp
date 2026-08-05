/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::store_failure::provider
{

struct provider_options_t
{
    std::string rid;
    std::string channel_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::string log_name;
    int heartbeat_ms;
    int lease_ttl_ms;
    int polling_ms;
    int grace_ms;

    static provider_options_t bind (const configuration_section_t &section)
    {
        return {.rid = section.require ("rid"),
                .channel_endpoint = section.require ("channelEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir"),
                .log_name = section.require ("logName"),
                .heartbeat_ms = std::stoi (section.require ("location.heartbeatMs")),
                .lease_ttl_ms = std::stoi (section.require ("location.leaseTtlMs")),
                .polling_ms = std::stoi (section.require ("location.pollingMs")),
                .grace_ms = std::stoi (section.require ("location.graceMs"))};
    }
};

inline provider_options_t read_provider_options (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("DiscoveryRegistryHa provider requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<provider_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::store_failure::provider
