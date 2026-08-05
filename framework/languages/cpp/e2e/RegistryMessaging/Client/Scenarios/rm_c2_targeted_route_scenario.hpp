/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace zlink::framework::e2e::registry_messaging::client
{

inline void run_rm_c2_targeted_route_scenario (const client_options_t &options)
{
    const auto provider_a_url = options.http_a_endpoint;
    auto topology = zlink::http_client::client_t::create ()
                      .base_url (provider_a_url)
                      .timeout (std::chrono::milliseconds (3000))
                      .build ();
    const auto topology_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    bool target_published = false;
    while (std::chrono::steady_clock::now () < topology_deadline) {
        const auto response =
          topology.get ("/locations/peers?mesh=" + std::string (route_channel))
            .submit<nlohmann::json> ()
            .result ();
        if (response) {
            for (const auto &peer : response.value ().body) {
                if (peer.value ("node_rid", "") == "api-b"
                    && peer.value ("role", "") == "router"
                    && peer.value ("ready", false)) {
                    target_published = true;
                    break;
                }
            }
        }
        if (target_published) {
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    ensure (target_published,
            "RM-C2 route topology snapshot did not publish api-b");

    const auto to_b = post_json<scenario_route_req_t, scenario_route_res_t> (
      provider_a_url, "/profile/route/request",
      scenario_route_req_t{.value = "target-b"},
      std::chrono::seconds (3));
    ensure (to_b.target_rid == "api-b", "RM-C2 target rid mismatch");

    const auto evidence_a = fetch_evidence (options.http_a_endpoint);
    const auto evidence_b = fetch_evidence (options.http_b_endpoint);
    bool found_on_a = false;
    bool found_on_b = false;
    for (const auto &entry : evidence_a.entries) {
        if (entry.marker == "ScenarioRouteReq" && entry.value == "target-b") {
            found_on_a = true;
        }
    }
    for (const auto &entry : evidence_b.entries) {
        if (entry.marker == "ScenarioRouteReq" && entry.value == "target-b") {
            found_on_b = true;
        }
    }
    ensure (!found_on_a, "RM-C2 target request reached the wrong provider");
    ensure (found_on_b, "RM-C2 target provider evidence was not recorded");

    auto missing = post_json<scenario_route_req_t, request_failure_res_t> (
      provider_a_url, "/profile/route/missing", scenario_route_req_t{.value = "missing"});
    ensure (missing.failed, "RM-C2 missing rid unexpectedly succeeded");
    ensure (missing.error_type == "NotFound",
            "RM-C2 missing rid error type mismatch: " + missing.error_type);
    std::cout << "scenario RM-C2 passed\n";
}

} // namespace zlink::framework::e2e::registry_messaging::client
