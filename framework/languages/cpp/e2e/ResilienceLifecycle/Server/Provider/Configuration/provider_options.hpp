/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

struct provider_options_t
{
    std::string rid;
    std::string instance_id;
    std::string api_endpoint;
    std::string route_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string evidence_file;
    std::string log_dir;
    std::optional<int> server_weight;

    static provider_options_t bind (const configuration_section_t &section)
    {
        const auto integer = [&] (std::string_view key) -> std::optional<int> {
            const auto value = section.get (key);
            return value ? std::optional<int> (std::stoi (*value)) : std::nullopt;
        };
        return {.rid = section.require ("rid"),
                .instance_id = section.require ("instanceId"),
                .api_endpoint = section.require ("apiEndpoint"),
                .route_endpoint = section.require ("routeEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .evidence_file = section.require ("evidenceFile"),
                .log_dir = section.require ("logDir"),
                .server_weight = integer ("serverWeight")};
    }
};

inline provider_options_t read_provider_options (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("ResilienceLifecycle provider requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<provider_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
