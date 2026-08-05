/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

struct client_options_t
{
    std::string log_dir;
    std::string scenario;
    std::string api_a_endpoint;
    std::string api_b_endpoint;
    std::string route_a_endpoint;
    std::string route_b_endpoint;
    std::string client_route_endpoint;
    std::string http_consumer_endpoint;
    std::string http_a_endpoint;
    std::string http_b_endpoint;
    std::string http_b_green_endpoint;
    std::string api_a_evidence_file;
    std::string api_b_evidence_file;
    std::string ready_file;
    std::string continue_file;
    std::string drained_file;
    std::string restore_file;
    std::string second_ready_file;
    std::string second_continue_file;
    std::string flap_phase;
    std::string flap_cycle;
};

inline client_options_t read_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown ResilienceLifecycle client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("ResilienceLifecycle client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open ResilienceLifecycle client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.log_dir = value ("logDir"),
            .scenario = value ("scenario"),
            .api_a_endpoint = value ("apiAEndpoint"),
            .api_b_endpoint = value ("apiBEndpoint"),
            .route_a_endpoint = value ("routeAEndpoint"),
            .route_b_endpoint = value ("routeBEndpoint"),
            .client_route_endpoint = value ("clientRouteEndpoint"),
            .http_consumer_endpoint = value ("httpConsumerEndpoint"),
            .http_a_endpoint = value ("httpAEndpoint"),
            .http_b_endpoint = value ("httpBEndpoint"),
            .http_b_green_endpoint = value ("httpBGreenEndpoint"),
            .api_a_evidence_file = value ("apiAEvidenceFile"),
            .api_b_evidence_file = value ("apiBEvidenceFile"),
            .ready_file = value ("readyFile"),
            .continue_file = value ("continueFile"),
            .drained_file = value ("drainedFile"),
            .restore_file = value ("restoreFile"),
            .second_ready_file = value ("secondReadyFile"),
            .second_continue_file = value ("secondContinueFile"),
            .flap_phase = value ("flapPhase"),
            .flap_cycle = value ("flapCycle")};
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
