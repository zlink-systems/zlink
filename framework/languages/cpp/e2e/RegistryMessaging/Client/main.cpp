/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/rm_a1_discovery_request_scenario.hpp"
#include "Scenarios/rm_a2_manual_endpoint_scenario.hpp"
#include "Scenarios/rm_a4_same_rid_failover_scenario.hpp"
#include "Scenarios/rm_a6_multiple_channels_scenario.hpp"
#include "Scenarios/rm_b1_scale_out_scenario.hpp"
#include "Scenarios/rm_b2_scale_in_scenario.hpp"
#include "Scenarios/rm_c1_request_send_scenario.hpp"
#include "Scenarios/rm_c2_targeted_route_scenario.hpp"
#include "Scenarios/rm_c3_multi_provider_distribution_scenario.hpp"
#include "Scenarios/rm_c4_timeout_isolation_scenario.hpp"
#include "Scenarios/rm_c5_missing_packet_scenario.hpp"
#include "Scenarios/rm_c7_weighted_provider_scenario.hpp"
#include "Scenarios/rm_c8_payload_round_trip_scenario.hpp"
#include "Scenarios/rm_c9_backpressure_scenario.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace rm_client = zlink::framework::e2e::registry_messaging::client;

int main (int argc, char **argv)
{
    const auto options = rm_client::read_client_options (argc, argv);
    const auto &scenario = options.scenario;
    if (scenario == "rm-a1") {
        rm_client::run_rm_a1_discovery_request_scenario (options);
    } else if (scenario == "rm-a2") {
        rm_client::run_rm_a2_manual_endpoint_scenario (options);
    } else if (scenario == "common") {
        rm_client::run_rm_a1_discovery_request_scenario (options);
        rm_client::run_rm_c1_request_send_scenario (options);
        rm_client::run_rm_a2_manual_endpoint_scenario (options);
        rm_client::run_rm_c3_multi_provider_distribution_scenario (options);
        rm_client::run_rm_a6_multiple_channels_scenario (options);
        rm_client::run_rm_c8_payload_round_trip_scenario (options);
        rm_client::run_rm_c2_targeted_route_scenario (options);
        rm_client::run_rm_c4_timeout_isolation_scenario (options);
        rm_client::run_rm_c5_missing_packet_scenario (options);
    } else if (scenario == "rm-a6") {
        rm_client::run_rm_a6_multiple_channels_scenario (options);
    } else if (scenario == "rm-c1") {
        rm_client::run_rm_c1_request_send_scenario (options);
    } else if (scenario == "rm-c2") {
        rm_client::run_rm_c2_targeted_route_scenario (options);
    } else if (scenario == "rm-c3") {
        rm_client::run_rm_c3_multi_provider_distribution_scenario (options);
    } else if (scenario == "rm-c4" || scenario == "timeout-cleanup") {
        rm_client::run_rm_c4_timeout_isolation_scenario (options);
    } else if (scenario == "rm-c5") {
        rm_client::run_rm_c5_missing_packet_scenario (options);
    } else if (scenario == "rm-b1" || scenario == "scale-out") {
        rm_client::run_rm_b1_scale_out_scenario (options);
    } else if (scenario == "rm-b2" || scenario == "scale-in") {
        rm_client::run_rm_b2_scale_in_scenario (options);
    } else if (scenario == "rm-a4" || scenario == "failover") {
        rm_client::run_rm_a4_same_rid_failover_scenario (options);
    } else if (scenario == "rm-c7" || scenario == "weighted") {
        rm_client::run_rm_c7_weighted_provider_scenario (options);
    } else if (scenario == "rm-c8") {
        rm_client::run_rm_c8_payload_round_trip_scenario (options);
    } else if (scenario == "rm-c9" || scenario == "backpressure") {
        rm_client::run_rm_c9_backpressure_scenario (options);
    } else {
        throw std::runtime_error ("unknown scenario " + scenario);
    }
    std::cout << "registry-messaging e2e result=passed\n";
    return 0;
}
