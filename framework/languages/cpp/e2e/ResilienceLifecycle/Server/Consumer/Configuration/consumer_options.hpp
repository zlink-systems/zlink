/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::consumer
{

inline std::vector<std::string> split_csv (const std::string &text)
{
    std::vector<std::string> result;
    std::stringstream input (text);
    std::string item;
    while (std::getline (input, item, ',')) {
        if (!item.empty ()) {
            result.push_back (item);
        }
    }
    return result;
}

struct consumer_options_t
{
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::vector<std::string> provider_endpoints;
    std::string log_dir;
    std::string trace_label;

    static consumer_options_t bind (const configuration_section_t &section)
    {
        return {.http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .provider_endpoints = split_csv (section.require ("providerEndpoints")),
                .log_dir = section.require ("logDir"),
                .trace_label = section.require ("traceLabel")};
    }
};

inline consumer_options_t read_consumer_options (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("ResilienceLifecycle consumer requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<consumer_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::resilience_lifecycle::consumer
