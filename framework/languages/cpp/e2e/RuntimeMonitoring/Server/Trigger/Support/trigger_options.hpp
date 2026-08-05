/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::trigger
{

struct trigger_options_t
{
    std::string rid;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string service_channel_endpoint;
    std::string service_b_channel_endpoint;
    std::string throw_channel_endpoint;
    std::string evidence_file;
    std::string log_dir;

    static trigger_options_t bind (const configuration_section_t &section)
    {
        return {.rid = section.require ("rid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .service_channel_endpoint = section.require ("serviceChannelEndpoint"),
                .service_b_channel_endpoint = section.require ("serviceBChannelEndpoint"),
                .throw_channel_endpoint = section.require ("throwChannelEndpoint"),
                .evidence_file = section.require ("evidenceFile"),
                .log_dir = section.require ("logDir")};
    }
};

inline trigger_options_t read_trigger_options (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("RuntimeMonitoring trigger requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<trigger_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::runtime_monitoring::trigger
