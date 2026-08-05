/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/rl_a1_provider_restart_scenario.hpp"
#include "Scenarios/rl_a2_provider_endpoint_remap_scenario.hpp"
#include "Scenarios/rl_a3_reconnect_storm_scenario.hpp"
#include "Scenarios/rl_a4_drain_and_green_endpoint_scenario.hpp"
#include "Scenarios/rl_a5_provider_flapping_scenario.hpp"
#include "Scenarios/rl_b1_cancellation_cleanup_scenario.hpp"
#include "Scenarios/rl_b2_crash_during_inflight_scenario.hpp"
#include "Scenarios/rl_b3_graceful_shutdown_scenario.hpp"
#include "Scenarios/rl_b4_runtime_drain_scenario.hpp"
#include "Scenarios/rl_b5_drain_inflight_scenario.hpp"
#include "Scenarios/rl_b6_gray_fault_scenario.hpp"
#include "Scenarios/rl_c1_client_host_lifecycle_scenario.hpp"
#include "Scenarios/rl_c3_node_pause_recovery_scenario.hpp"
#include "Scenarios/rl_c4_location_store_outage_scenario.hpp"
#include "Scenarios/rl_d2_observer_fault_scenario.hpp"
#include "Scenarios/rl_d3_dispatch_error_evidence_scenario.hpp"
#include "Scenarios/rl_d4_missing_request_handler_scenario.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace rl_client = zlink::framework::e2e::resilience_lifecycle::client;

int main (int argc, char **argv)
{
    (void) argc;
    (void) argv;

    try {
        const auto options = rl_client::read_client_options (argc, argv);
        const auto &scenario = options.scenario;
        if (scenario == "inflight-crash") {
            rl_client::run_inflight_crash_scenario (options);
        } else if (scenario == "location-store-outage") {
            rl_client::run_location_store_outage_scenario (options);
        } else if (scenario == "location-store-recovered") {
            rl_client::run_location_store_recovered_scenario (options);
        } else if (scenario == "observer-fault") {
            rl_client::run_observer_fault_scenario (options);
        } else if (scenario == "rl-a1") {
            rl_client::run_rl_a1_provider_restart_scenario (options);
        } else if (scenario == "rl-a2") {
            rl_client::run_rl_a2_provider_endpoint_remap_scenario (options);
        } else if (scenario == "rl-a3") {
            rl_client::run_rl_a3_reconnect_storm_probe (options);
        } else if (scenario == "rl-a4") {
            rl_client::run_rl_a4_drain_and_green_endpoint_scenario (options);
        } else if (scenario == "rl-b4") {
            rl_client::run_rl_b4_runtime_drain_scenario (options);
        } else if (scenario == "rl-b5") {
            rl_client::run_rl_b5_drain_inflight_scenario (options);
        } else if (scenario == "rl-a5") {
            rl_client::run_rl_a5_provider_flapping_probe (options);
        } else if (scenario == "rl-b1") {
            rl_client::run_rl_b1_cancellation_cleanup_scenario (options);
        } else if (scenario == "rl-b3") {
            rl_client::run_rl_b3_graceful_shutdown_scenario (options);
        } else if (scenario == "rl-b6") {
            rl_client::run_rl_b6_gray_fault_scenario (options);
        } else if (scenario == "rl-c1") {
            rl_client::run_rl_c1_client_host_lifecycle_probe (options);
        } else if (scenario == "rl-c3") {
            rl_client::run_rl_c3_node_pause_recovery_probe (options);
        } else if (scenario == "rl-d3") {
            rl_client::run_rl_d3_dispatch_error_evidence_scenario (options);
        } else if (scenario == "rl-d4") {
            rl_client::run_rl_d4_missing_request_handler_scenario (options);
        } else {
            throw std::runtime_error ("unknown scenario " + scenario);
        }
    }
    catch (const std::exception &error) {
        std::cerr << "resilience-lifecycle scenario failed: " << error.what () << "\n";
        return 1;
    }
    std::cout << "resilience-lifecycle client result=passed\n";
    return 0;
}
