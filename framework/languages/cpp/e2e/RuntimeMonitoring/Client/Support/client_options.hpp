/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::e2e::runtime_monitoring::client
{

struct client_options_t
{
    std::string scenario;
    std::string service_url;
    std::string filtered_service_url;
    std::string throw_service_url;
    std::string trigger_url;
    std::string log_dir;
    std::string old_service_channel_endpoint;
    std::string new_service_channel_endpoint;
};

inline client_options_t read_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown RuntimeMonitoring client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("RuntimeMonitoring client requires --config=<path>");
    }

    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open RuntimeMonitoring client config: " + path);
    }

    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto required = [&] (const char *key) {
        const auto found = section.find (key);
        if (found == section.end () || !found->is_string () || found->get<std::string> ().empty ()) {
            throw std::runtime_error (std::string ("RuntimeMonitoring client config requires ")
                                      + key);
        }
        return found->get<std::string> ();
    };
    const auto optional = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };

    client_options_t options{.scenario = required ("scenario"),
                             .service_url = required ("serviceUrl"),
                             .filtered_service_url = required ("filteredServiceUrl"),
                             .throw_service_url = optional ("throwServiceUrl"),
                             .trigger_url = required ("triggerUrl"),
                             .log_dir = required ("logDir"),
                             .old_service_channel_endpoint =
                               optional ("oldServiceChannelEndpoint"),
                             .new_service_channel_endpoint =
                               optional ("newServiceChannelEndpoint")};
    return options;
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
