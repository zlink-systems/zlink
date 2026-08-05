/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::registration_codec::server
{

struct server_options_t
{
    std::string log_dir;
    std::string api_endpoint;
    std::string http_endpoint;
    std::string invalid_mode;
    std::string server_mode;

    static server_options_t bind (const configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .api_endpoint = section.require ("apiEndpoint"),
                .http_endpoint = section.get ("httpEndpoint").value_or (""),
                .invalid_mode = section.get ("invalidMode").value_or (""),
                .server_mode = section.get ("serverMode").value_or ("main")};
    }
};

inline server_options_t read_server_options (app_t &app, int argc, char **argv,
                                             const char *role)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error (std::string ("RegistrationCodec ") + role
                                  + " requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<server_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::registration_codec::server
