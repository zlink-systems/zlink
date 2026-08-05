/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/ta_a1_scenario.hpp"
#include "Scenarios/ta_a2_scenario.hpp"
#include "Scenarios/ta_a3_scenario.hpp"
#include "Scenarios/ta_a4_scenario.hpp"
#include "Scenarios/ta_b1_scenario.hpp"
#include "Scenarios/ta_b2_scenario.hpp"
#include "Scenarios/ta_b3_scenario.hpp"

int main (int argc, char **argv)
{
    const auto configuration = parse_client_configuration (argc, argv);
    auto actor = make_http (configuration.actor_http);
    auto actor_b = make_http (configuration.actor_b_http);
    auto caller = make_http (configuration.caller_http);
    auto route_control = make_http (configuration.route_control_http);
    auto session_a = make_http (configuration.session_a_http);
    auto session_b = make_http (configuration.session_b_http);
    const auto selected = split_selector (configuration.scenario);
    validate_selector (selected);

    if (should_run (selected, {"TA-A1", "ta-a1"})) {
        run_ta_a1_scenario (actor, caller, session_a, configuration.session_a_stream);
    }
    if (should_run (selected, {"TA-A2", "ta-a2"})) {
        run_ta_a2_scenario (actor, caller, session_a, session_b);
    }
    if (should_run (selected, {"TA-A3", "ta-a3"})) {
        run_ta_a3_scenario (actor, caller, session_b, configuration.session_b_stream);
    }
    if (should_run (selected, {"TA-A4", "ta-a4"})) {
        run_ta_a4_scenario (actor, caller, session_a, configuration.session_a_stream);
    }
    if (should_run (selected, {"TA-B1", "ta-b1"})) {
        run_ta_b1_scenario (actor, caller);
    }
    if (should_run (selected, {"TA-B2", "ta-b2"})) {
        run_ta_b2_scenario (actor, actor_b, caller);
    }
    if (should_run (selected, {"TA-B3", "ta-b3"})) {
        run_ta_b3_scenario (actor, caller, route_control);
    }

    std::cout << "to-actor-messaging e2e result=passed\n";
    return 0;
}
