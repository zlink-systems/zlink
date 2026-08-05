/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/mon_a1_socket_events_scenario.hpp"
#include "Scenarios/mon_a2_location_events_scenario.hpp"
#include "Scenarios/mon_a3_spot_events_scenario.hpp"
#include "Scenarios/mon_a4_availability_transition_scenario.hpp"
#include "Scenarios/mon_a5_fixed_kinds_scenario.hpp"
#include "Scenarios/mon_b_publish_monitoring_absence_scenario.hpp"
#include "Scenarios/mon_c1_dispatch_failure_scenario.hpp"
#include "Scenarios/mon_d1_failure_recovery_scenario.hpp"
#include "Support/client_options.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace rm_client = zlink::framework::e2e::runtime_monitoring::client;

namespace
{

using rm_client::client_options_t;

int run_scenarios (const client_options_t &client_options)
{
    const auto &scenario = client_options.scenario;
    if (client_options.scenario == "mon-d1") {
        rm_client::run_mon_d1_failure_recovery_scenario (client_options);
    } else {
        auto wants = [&scenario] (const char *id) {
            return scenario.empty () || scenario == "all" || scenario == id;
        };
        if (wants ("mon-a1")) {
            rm_client::run_mon_a1_socket_events_scenario (client_options);
        }
        if (wants ("mon-a2")) {
            rm_client::run_mon_a2_location_events_scenario (client_options);
        }
        if (wants ("mon-a3")) {
            rm_client::run_mon_a3_spot_events_scenario (client_options);
        }
        if (wants ("mon-a5")) {
            rm_client::run_mon_a5_fixed_kinds_scenario (client_options);
        }
        if (wants ("mon-b1")) {
            rm_client::run_mon_b1_publish_monitoring_absence_scenario (client_options);
        }
        if (wants ("mon-b2")) {
            rm_client::run_mon_b2_publish_monitoring_absence_scenario (client_options);
        }
        if (scenario == "mon-a4") {
            rm_client::run_mon_a4_availability_transition_scenario (client_options);
        }
        if (wants ("mon-c1")) {
            rm_client::run_mon_c1_dispatch_failure_scenario (client_options);
        }
        if (!wants ("mon-a1") && !wants ("mon-a2") && !wants ("mon-a3") && !wants ("mon-a5")
            && !wants ("mon-a4") && !wants ("mon-b1") && !wants ("mon-b2")
            && !wants ("mon-c1")) {
            throw std::runtime_error ("unknown RuntimeMonitoring scenario: " + scenario);
        }
    }
    return 0;
}

} // namespace

int main (int argc, char **argv)
{
    try {
        auto client_options = rm_client::read_client_options (argc, argv);
        run_scenarios (client_options);
    }
    catch (const std::exception &ex) {
        std::cerr << "runtime-monitoring client failed: " << ex.what () << '\n';
        return 1;
    }
    std::cout << "runtime-monitoring client result=passed" << '\n';
    return 0;
}
