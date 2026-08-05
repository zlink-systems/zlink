/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/spot_service_contracts.hpp"
#include "Scenarios/sm_a1_scenario.hpp"
#include "Scenarios/sm_a2_scenario.hpp"
#include "Scenarios/sm_a3_scenario.hpp"
#include "Scenarios/sm_a4_scenario.hpp"
#include "Scenarios/sm_a5_scenario.hpp"
#include "Scenarios/sm_a6_scenario.hpp"
#include "Scenarios/sm_a7_scenario.hpp"
#include "Scenarios/sm_a8_scenario.hpp"
#include "Scenarios/sm_b1_scenario.hpp"
#include "Scenarios/sm_b2_scenario.hpp"
#include "Scenarios/sm_b3_scenario.hpp"
#include "Scenarios/sm_b4_scenario.hpp"
#include "Scenarios/sm_b5_scenario.hpp"
#include "Scenarios/sm_b6_scenario.hpp"
#include "Scenarios/sm_b7_scenario.hpp"
#include "Scenarios/sm_b8_scenario.hpp"
#include "Scenarios/sm_b9_scenario.hpp"
#include "Scenarios/sm_c1_scenario.hpp"
#include "Scenarios/sm_c2_scenario.hpp"
#include "Scenarios/sm_c3_scenario.hpp"
#include "Scenarios/sm_c4_scenario.hpp"
#include "Scenarios/sm_c5_scenario.hpp"
#include "Scenarios/sm_d1_scenario.hpp"
#include "Scenarios/sm_d2_scenario.hpp"
#include "Scenarios/sm_d3_scenario.hpp"
#include "Scenarios/sm_d4_scenario.hpp"
#include "Scenarios/sm_d5_scenario.hpp"
#include "Scenarios/sm_d6_scenario.hpp"
#include "Scenarios/sm_d7_scenario.hpp"
#include "Scenarios/sm_d8_scenario.hpp"
#include "Scenarios/sm_d9_scenario.hpp"
#include "Scenarios/sm_d10_scenario.hpp"
#include "Scenarios/sm_d11_scenario.hpp"
#include "Scenarios/sm_d12_scenario.hpp"
#include "Scenarios/sm_d13_scenario.hpp"
#include "Scenarios/sm_d14_scenario.hpp"
#include "Scenarios/sm_d15_scenario.hpp"
#include "Scenarios/sm_e1_scenario.hpp"
#include "Scenarios/sm_e2_scenario.hpp"
#include "Scenarios/sm_e3_scenario.hpp"
#include "Scenarios/sm_e4_scenario.hpp"
#include "Scenarios/sm_f1_scenario.hpp"
#include "Scenarios/sm_f2_scenario.hpp"
#include "Scenarios/sm_f3_scenario.hpp"
#include "Scenarios/sm_f4_scenario.hpp"
#include "Scenarios/sm_f5_scenario.hpp"
#include "Scenarios/sm_f6_scenario.hpp"
#include "Scenarios/sm_g1_scenario.hpp"
#include "Scenarios/sm_g2_scenario.hpp"
#include "Scenarios/sm_g3_scenario.hpp"
#include "Scenarios/sm_g4_scenario.hpp"
#include "Scenarios/sm_q9_scenario.hpp"
#include "Support/client_options.hpp"

#include <zlink/http_client.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace e2e = zlink::framework::e2e::spot_service;
namespace e2e_client = zlink::framework::e2e::spot_service::client;
namespace scenarios = zlink::framework::e2e::spot_service::client::scenarios;

namespace
{

zlink::http_client::client_t make_http (const std::string &base_url)
{
    if (base_url.empty ()) {
        throw std::runtime_error ("SpotService client HTTP endpoint is required");
    }
    return zlink::http_client::client_t::create ()
      .base_url (base_url)
      .timeout (std::chrono::minutes (5))
      .build ();
}

template <typename TReply, typename TRequest>
TReply post_json (const std::string &base_url, const std::string &path, const TRequest &request)
{
    auto raw = make_http (base_url).post (path).body (request).submit_raw ().result ();
    if (!raw || raw.value ().status >= 400) {
        const auto error = raw ? raw.value ().body
                               : (raw.error () ? raw.error ()->what () : "HTTP failed");
        throw std::runtime_error ("POST " + path + " failed: " + error);
    }
    return nlohmann::json::parse (raw.value ().body).template get<TReply> ();
}

e2e::join_res_t ensure_f_spot (const std::string &play_http_endpoint)
{
    const auto response =
      post_json<e2e::remote_actor_flow_res_t> (
        play_http_endpoint, "/spot/remote-actor",
        e2e::remote_actor_flow_req_t{
          .join = e2e::join_req_t{.key = "b-room",
                                  .actor_id = "bob",
                                  .display_name = "Bob",
                                  .level = 9,
                                  .tags = {"beta", "remote"}},
          .state = e2e::state_req_t{.op = "add", .amount = 11}});
    if (response.join.owner_node_rid != "play-b"
        || response.join.spot_id != "user:play-b:b-room") {
        throw std::runtime_error ("F scenario setup did not create play-b b-room spot");
    }
    auto api = make_http (play_http_endpoint);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    for (;;) {
        auto visible =
          api.post ("/spot/direct")
            .body (e2e::direct_spot_route_req_t{.target_node_rid = "play-b",
                                                .spot_id = response.join.spot_id,
                                                .value = "f-setup-ready",
                                                .source_actor_id = "external-client"})
            .submit_raw ()
            .result ();
        if (visible && visible.value ().status < 400) {
            break;
        }
        const bool location_pending =
          visible && visible.value ().status == 500
          && visible.value ().body.find ("no live location row") != std::string::npos;
        if (!location_pending || std::chrono::steady_clock::now () >= deadline) {
            const auto error = visible ? visible.value ().body
                                       : (visible.error () ? visible.error ()->what ()
                                                          : "HTTP failed");
            throw std::runtime_error ("F scenario location readiness failed: " + error);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    return response.join;
}

void run_route_ready (const e2e_client::client_options_t &options,
                      bool require_play_a,
                      bool require_play_b)
{
    if (require_play_a) {
        const auto reply = post_json<e2e::channel_control_ping_res_t> (
          options.play_http_endpoint, "/channel/control-ping",
          e2e::channel_control_ping_req_t{.target_node_rid = "play-a",
                                          .value = "route-ready-play-a"});
        if (reply.node_rid != "play-a") {
            throw std::runtime_error ("route-ready play-a reply mismatch");
        }
    }
    if (require_play_b) {
        const auto reply = post_json<e2e::channel_control_ping_res_t> (
          options.play_b_http_endpoint, "/channel/control-ping",
          e2e::channel_control_ping_req_t{.target_node_rid = "play-b",
                                          .value = "route-ready-play-b"});
        if (reply.node_rid != "play-b") {
            throw std::runtime_error ("route-ready play-b reply mismatch");
        }
    }
}

void run_scenario (const e2e_client::client_options_t &options)
{
    const auto &mode = options.scenario_mode;

    if (mode == "route-ready") {
        run_route_ready (options, true, true);
        std::cout << "scenario route-ready passed\n";
    } else if (mode == "route-ready-play-a") {
        run_route_ready (options, true, false);
        std::cout << "scenario route-ready-play-a passed\n";
    } else if (mode == "route-ready-play-b") {
        run_route_ready (options, false, true);
        std::cout << "scenario route-ready-play-b passed\n";
    } else if (mode == "sm-a1-a2-a4-f1-f2") {
        e2e_client::spot_lifecycle_order_context_t context;
        scenarios::run_sm_a1_scenario (options.play_http_endpoint, context);
        std::cout << "scenario SM-A1 passed\n";
        scenarios::run_sm_a4_scenario (options.play_b_http_endpoint, context);
        std::cout << "scenario SM-A4 passed\n";
        scenarios::run_sm_f1_scenario (options.play_b_http_endpoint, context);
        std::cout << "scenario SM-F1 passed\n";
        scenarios::run_sm_f2_scenario (options.play_b_http_endpoint, context);
        std::cout << "scenario SM-F2 passed\n";
        scenarios::run_sm_a2_scenario (options.play_http_endpoint, context);
        std::cout << "scenario SM-A2 passed\n";
    } else if (mode == "sm-a1") {
        scenarios::run_sm_a1_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-A1 passed\n";
    } else if (mode == "sm-a2") {
        scenarios::run_sm_a2_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-A2 passed\n";
    } else if (mode == "sm-a3") {
        scenarios::run_sm_a3_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-A3 passed\n";
    } else if (mode == "sm-a4") {
        scenarios::run_sm_a4_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-A4 passed\n";
    } else if (mode == "sm-a5") {
        scenarios::run_sm_a5_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-A5 passed\n";
    } else if (mode == "sm-a6") {
        scenarios::run_sm_a6_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-A6 passed\n";
    } else if (mode == "sm-a7") {
        scenarios::run_sm_a7_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-A7 passed\n";
    } else if (mode == "sm-a8") {
        scenarios::run_sm_a8_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-A8 passed\n";
    } else if (mode == "sm-b1") {
        scenarios::run_sm_b1_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-B1 passed\n";
    } else if (mode == "sm-b2") {
        scenarios::run_sm_b2_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-B2 passed\n";
    } else if (mode == "sm-b3") {
        scenarios::run_sm_b3_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-B3 passed\n";
    } else if (mode == "sm-b4") {
        scenarios::run_sm_b4_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-B4 passed\n";
    } else if (mode == "sm-b5") {
        scenarios::run_sm_b5_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-B5 passed\n";
    } else if (mode == "sm-b6") {
        scenarios::run_sm_b6_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-B6 passed\n";
    } else if (mode == "sm-b7") {
        scenarios::run_sm_b7_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-B7 passed\n";
    } else if (mode == "sm-b8") {
        scenarios::run_sm_b8_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-B8 passed\n";
    } else if (mode == "sm-b9") {
        scenarios::run_sm_b9_scenario (
          options.play_http_endpoint, options.play_b_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-B9 passed\n";
    } else if (mode == "sm-c1") {
        scenarios::run_sm_c1_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-C1 passed\n";
    } else if (mode == "sm-c2") {
        scenarios::run_sm_c2_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-C2 passed\n";
    } else if (mode == "sm-c3") {
        scenarios::run_sm_c3_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-C3 passed\n";
    } else if (mode == "sm-c4") {
        scenarios::run_sm_c4_scenario (options.play_http_endpoint, options.gateway_http_endpoint);
        std::cout << "scenario SM-C4 passed\n";
    } else if (mode == "sm-c5") {
        scenarios::run_sm_c5_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-C5 passed\n";
    } else if (mode == "sm-d1") {
        scenarios::run_sm_d1_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-D1 passed\n";
    } else if (mode == "sm-d2") {
        scenarios::run_sm_d2_scenario (options.play_b_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-D2 passed\n";
    } else if (mode == "sm-d3") {
        scenarios::run_sm_d3_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-D3 passed\n";
    } else if (mode == "sm-d4") {
        scenarios::run_sm_d4_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D4 passed\n";
    } else if (mode == "sm-d5") {
        scenarios::run_sm_d5_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D5 passed\n";
    } else if (mode == "sm-d6") {
        scenarios::run_sm_d6_scenario (
          options.stream_endpoint, options.alternate_stream_endpoint, options.play_http_endpoint);
        std::cout << "scenario SM-D6 passed\n";
    } else if (mode == "sm-d7") {
        scenarios::run_sm_d7_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D7 passed\n";
    } else if (mode == "sm-d8") {
        scenarios::run_sm_d8_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D8 passed\n";
    } else if (mode == "sm-d9") {
        scenarios::run_sm_d9_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D9 passed\n";
    } else if (mode == "sm-d10") {
        scenarios::run_sm_d10_scenario (
          options.stream_endpoint, options.alternate_stream_endpoint, options.play_http_endpoint,
          options.play_b_http_endpoint);
        std::cout << "scenario SM-D10 passed\n";
    } else if (mode == "sm-d11") {
        scenarios::run_sm_d11_scenario (options.session_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-D11 passed\n";
    } else if (mode == "sm-d12") {
        scenarios::run_sm_d12_scenario (options.stream_endpoint, options.alternate_stream_endpoint);
        std::cout << "scenario SM-D12 passed\n";
    } else if (mode == "sm-d13") {
        scenarios::run_sm_d13_scenario (options.stream_endpoint);
        std::cout << "scenario SM-D13 passed\n";
    } else if (mode == "sm-d14") {
        scenarios::run_sm_d14_scenario (options.tls_stream_endpoint);
        std::cout << "scenario SM-D14 passed\n";
    } else if (mode == "sm-d15") {
        scenarios::run_sm_d15_scenario (
          options.play_http_endpoint, options.gateway_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-D15 passed\n";
    } else if (mode == "sm-e1") {
        scenarios::run_sm_e1_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-E1 passed\n";
    } else if (mode == "sm-e2") {
        scenarios::run_sm_e2_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-E2 passed\n";
    } else if (mode == "sm-e3") {
        scenarios::run_sm_e3_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-E3 passed\n";
    } else if (mode == "sm-e4") {
        scenarios::run_sm_e4_scenario (options.play_http_endpoint);
        std::cout << "scenario SM-E4 passed\n";
    } else if (mode == "sm-f1") {
        const auto spot = ensure_f_spot (options.play_http_endpoint);
        scenarios::run_sm_f1_scenario (options.play_http_endpoint, spot.spot_id);
    } else if (mode == "sm-f2") {
        const auto spot = ensure_f_spot (options.play_http_endpoint);
        scenarios::run_sm_f1_scenario (options.play_http_endpoint, spot.spot_id);
        scenarios::run_sm_f2_scenario (options.play_http_endpoint, spot.spot_id);
    } else if (mode == "sm-f3") {
        const auto spot = ensure_f_spot (options.play_http_endpoint);
        scenarios::run_sm_f1_scenario (options.play_http_endpoint, spot.spot_id);
        scenarios::run_sm_f2_scenario (options.play_http_endpoint, spot.spot_id);
        scenarios::run_sm_f3_scenario (options.play_http_endpoint, options.play_b_http_endpoint,
                                       spot.spot_id);
    } else if (mode == "sm-f4") {
        const auto spot = ensure_f_spot (options.play_http_endpoint);
        scenarios::run_sm_f4_scenario (options.play_http_endpoint,
                                       options.play_b_http_endpoint, spot.spot_id);
    } else if (mode == "sm-f5") {
        const auto spot = ensure_f_spot (options.play_http_endpoint);
        scenarios::run_sm_f4_scenario (options.play_http_endpoint,
                                       options.play_b_http_endpoint, spot.spot_id);
        scenarios::run_sm_f5_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
    } else if (mode == "sm-f6") {
        scenarios::run_sm_f6_scenario (options.multi_a_http_endpoint,
                                       options.multi_b_http_endpoint,
                                       options.run_id);
        std::cout << "scenario SM-F6 passed\n";
    } else if (mode == "sm-g1" || mode == "crash-setup") {
        scenarios::run_sm_g1_crash_observation_scenario (
          options.stream_endpoint, options.alternate_stream_endpoint, options.crash_ready_file,
          options.crash_go_file, options.crash_observed_file);
    } else if (mode == "crash-recover") {
        scenarios::run_sm_g1_crash_recovery_scenario (options.stream_endpoint);
    } else if (mode == "sm-g2") {
        scenarios::run_sm_g2_scenario (options.play_http_endpoint, options.play_b_http_endpoint);
        std::cout << "scenario SM-G2 passed\n";
    } else if (mode == "sm-g3") {
        scenarios::run_sm_g3_scenario (options.play_http_endpoint, options.stream_endpoint);
        std::cout << "scenario SM-G3 passed\n";
    } else if (mode == "sm-g4") {
        scenarios::run_sm_g4_scenario (options.stream_endpoint);
        std::cout << "scenario SM-G4 passed\n";
    } else if (mode == "sm-q9") {
        scenarios::run_sm_q9_scenario (options.multi_a_http_endpoint,
                                       options.multi_b_http_endpoint,
                                       options.multi_a_request_http_endpoint,
                                       options.multi_b_request_http_endpoint);
    } else {
        throw std::runtime_error ("unsupported SpotService scenario mode: " + mode);
    }
}

} // namespace

int main (int argc, char **argv)
{
    try {
        run_scenario (e2e_client::client_options_t::from_config (argc, argv));
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "spot-service scenario failed: " << error.what () << std::endl;
        return 1;
    }
}
