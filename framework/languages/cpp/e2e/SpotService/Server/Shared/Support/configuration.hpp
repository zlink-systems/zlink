/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

inline std::vector<std::string> split_endpoints (const std::string &text)
{
    std::vector<std::string> endpoints;
    std::stringstream input (text);
    std::string endpoint;
    while (std::getline (input, endpoint, ',')) {
        if (!endpoint.empty ()) {
            endpoints.push_back (endpoint);
        }
    }
    return endpoints;
}

inline void load_spot_service_config (zlink::framework::app_t &app,
                                      int argc,
                                      char **argv,
                                      const char *role)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error (std::string ("SpotService ") + role
                                  + " requires --config=<path>");
    }
    app.config ().load_json (*path);
}
