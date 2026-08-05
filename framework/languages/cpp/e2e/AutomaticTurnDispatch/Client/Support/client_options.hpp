/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

struct client_options_t
{
    std::string session_a_stream_endpoint;
    std::string session_b_stream_endpoint;
    std::string scenario;
    std::string request_id;
    std::string spot_id;
};

inline client_options_t parse_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown AutomaticTurnDispatch client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("AutomaticTurnDispatch client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open AutomaticTurnDispatch client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.session_a_stream_endpoint = value ("sessionAStreamEndpoint"),
            .session_b_stream_endpoint = value ("sessionBStreamEndpoint"),
            .scenario = value ("scenario"),
            .request_id = value ("requestId"),
            .spot_id = value ("spotId")};
}

inline zlink::stream_connector::connector_options_t
make_connector_options (const client_options_t &options)
{
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = options.session_a_stream_endpoint;
    connector_options.connect_timeout = std::chrono::milliseconds (3000);
    connector_options.request_timeout = std::chrono::milliseconds (10000);
    connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    return connector_options;
}

inline zlink::stream_connector::connector_options_t
make_connector_options (std::string endpoint)
{
    return make_connector_options (client_options_t{.session_a_stream_endpoint =
                                                      std::move (endpoint)});
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
