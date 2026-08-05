/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "client_support.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::e2e::spot_service::client
{

struct client_options_t
{
    std::string log_dir;
    std::string route_endpoint;
    std::string route_a_endpoint;
    std::string route_b_endpoint;
    std::string multi_route_client_a_endpoint;
    std::string multi_route_client_b_endpoint;
    std::string multi_route_a_endpoint;
    std::string multi_route_b_endpoint;
    std::string spot_router_endpoint;
    std::string pubsub_endpoint;
    std::string publisher_endpoint;
    std::string api_endpoint;
    std::string stream_endpoint;
    std::string alternate_stream_endpoint;
    std::string tls_stream_endpoint;
    std::string scenario_mode;
    std::string run_id;
    std::string play_http_endpoint;
    std::string play_b_http_endpoint;
    std::string multi_a_http_endpoint;
    std::string multi_b_http_endpoint;
    std::string multi_a_request_http_endpoint;
    std::string multi_b_request_http_endpoint;
    std::string session_http_endpoint;
    std::string gateway_http_endpoint;
    std::string client_rid;
    std::string crash_ready_file;
    std::string crash_go_file;
    std::string crash_observed_file;

    static client_options_t from_config (int argc, char **argv)
    {
        std::string path;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            constexpr std::string_view prefix = "--config=";
            if (argument.rfind (prefix, 0) != 0) {
                throw std::runtime_error ("unknown SpotService client option: " + argument);
            }
            path = argument.substr (prefix.size ());
        }
        if (path.empty ()) {
            throw std::runtime_error ("SpotService client requires --config=<path>");
        }
        std::ifstream input (path);
        if (!input) {
            throw std::runtime_error ("cannot open SpotService client config: " + path);
        }
        const auto section = nlohmann::json::parse (input).at ("e2e");
        const auto value = [&] (const char *key) {
            const auto found = section.find (key);
            return found == section.end () ? std::string{} : found->get<std::string> ();
        };
        return {.log_dir = value ("logDir"),
                .route_endpoint = value ("routeEndpoint"),
                .route_a_endpoint = value ("routeAEndpoint"),
                .route_b_endpoint = value ("routeBEndpoint"),
                .multi_route_client_a_endpoint = value ("multiRouteClientAEndpoint"),
                .multi_route_client_b_endpoint = value ("multiRouteClientBEndpoint"),
                .multi_route_a_endpoint = value ("multiRouteAEndpoint"),
                .multi_route_b_endpoint = value ("multiRouteBEndpoint"),
                .spot_router_endpoint = value ("spotRouterEndpoint"),
                .pubsub_endpoint = value ("pubsubEndpoint"),
                .publisher_endpoint = value ("publisherEndpoint"),
                .api_endpoint = value ("apiEndpoint"),
                .stream_endpoint = value ("streamEndpoint"),
                .alternate_stream_endpoint = value ("alternateStreamEndpoint"),
                .tls_stream_endpoint = value ("tlsStreamEndpoint"),
                .scenario_mode = value ("scenarioMode"),
                .run_id = value ("runId"),
                .play_http_endpoint = value ("playHttpEndpoint"),
                .play_b_http_endpoint = value ("playBHttpEndpoint"),
                .multi_a_http_endpoint = value ("multiAHttpEndpoint"),
                .multi_b_http_endpoint = value ("multiBHttpEndpoint"),
                .multi_a_request_http_endpoint = value ("multiARequestHttpEndpoint"),
                .multi_b_request_http_endpoint = value ("multiBRequestHttpEndpoint"),
                .session_http_endpoint = value ("sessionHttpEndpoint"),
                .gateway_http_endpoint = value ("gatewayHttpEndpoint"),
                .client_rid = section.value ("clientRid", "client-session"),
                .crash_ready_file = value ("crashReadyFile"),
                .crash_go_file = value ("crashGoFile"),
                .crash_observed_file = value ("crashObservedFile")};
    }
};

} // namespace zlink::framework::e2e::spot_service::client
