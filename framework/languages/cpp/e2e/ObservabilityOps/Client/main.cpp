/* SPDX-License-Identifier: MPL-2.0 */

#include "Scenarios/obs_a1_scenario.hpp"
#include "Scenarios/obs_a2_scenario.hpp"
#include "Scenarios/obs_a3_scenario.hpp"
#include "Scenarios/obs_a4_scenario.hpp"
#include "Scenarios/obs_b1_scenario.hpp"
#include "Scenarios/obs_b2_scenario.hpp"
#include "Scenarios/obs_b3_scenario.hpp"
#include "Scenarios/obs_b4_scenario.hpp"
#include "Scenarios/obs_c1_scenario.hpp"
#include "Scenarios/obs_c2_scenario.hpp"
#include "Scenarios/obs_c3_scenario.hpp"
#include "Scenarios/obs_c4_scenario.hpp"
#include "Scenarios/obs_c5_scenario.hpp"
#include "Support/client_runner.hpp"

#include <iostream>

namespace client = zlink::framework::e2e::observability_ops::client;
namespace scenarios = zlink::framework::e2e::observability_ops::client::scenarios;

int client::run_scenario_verification (const verification_input_t &input)
{
    if (input.scenario_id == "OBS-A1") {
        scenarios::run_obs_a1_scenario (input);
    } else if (input.scenario_id == "OBS-A2") {
        scenarios::run_obs_a2_scenario (input);
    } else if (input.scenario_id == "OBS-A3") {
        scenarios::run_obs_a3_scenario (input);
    } else if (input.scenario_id == "OBS-A4") {
        scenarios::run_obs_a4_scenario (input);
    } else if (input.scenario_id == "OBS-B1") {
        scenarios::run_obs_b1_scenario (input);
    } else if (input.scenario_id == "OBS-B2") {
        scenarios::run_obs_b2_scenario (input);
    } else if (input.scenario_id == "OBS-B3") {
        scenarios::run_obs_b3_scenario (input);
    } else if (input.scenario_id == "OBS-B4") {
        scenarios::run_obs_b4_scenario (input);
    } else if (input.scenario_id == "OBS-C1") {
        scenarios::run_obs_c1_scenario (input);
    } else if (input.scenario_id == "OBS-C2") {
        scenarios::run_obs_c2_scenario (input);
    } else if (input.scenario_id == "OBS-C3") {
        scenarios::run_obs_c3_scenario (input);
    } else if (input.scenario_id == "OBS-C4") {
        scenarios::run_obs_c4_scenario (input);
    } else if (input.scenario_id == "OBS-C5") {
        scenarios::run_obs_c5_scenario (input);
    } else {
        throw std::runtime_error ("unknown ObservabilityOps verification scenario: "
                                  + input.scenario_id);
    }
    std::cout << input.scenario_id << " client verification PASS\n";
    return 0;
}

int main (int argc, char **argv)
{
    return zlink::framework::e2e::observability_ops::client::run (argc, argv);
}
