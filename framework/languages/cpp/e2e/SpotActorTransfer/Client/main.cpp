/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Support/scenario_runner_support.hpp"
#include "Scenarios/st_a1_scenario.hpp"
#include "Scenarios/st_a2_scenario.hpp"
#include "Scenarios/st_a3_scenario.hpp"
#include "Scenarios/st_b1_scenario.hpp"
#include "Scenarios/st_b2_scenario.hpp"
#include "Scenarios/st_b3_scenario.hpp"
#include "Scenarios/st_b4_scenario.hpp"
#include "Scenarios/st_c1_scenario.hpp"
#include "Scenarios/st_c2_scenario.hpp"
#include "Scenarios/st_c3_scenario.hpp"
#include "Scenarios/st_d1_scenario.hpp"
#include "Scenarios/st_d2_scenario.hpp"
#include "Scenarios/st_e1_scenario.hpp"
#include "Scenarios/st_e2_scenario.hpp"
#include "Scenarios/st_f1_scenario.hpp"
#include "Scenarios/st_f2_scenario.hpp"
#include "Scenarios/st_f3_scenario.hpp"
#include "Scenarios/st_f4_scenario.hpp"
#include "Scenarios/st_f5_scenario.hpp"
#include "Scenarios/st_f6_scenario.hpp"

namespace
{

std::vector<std::string> selected_scenarios (const std::string &raw)
{
    std::vector<std::string> names;
    std::stringstream stream (raw);
    std::string token;
    while (std::getline (stream, token, ',')) {
        if (!token.empty ()) {
            names.push_back (token);
        }
    }
    return names;
}

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = read_options (argc, argv);
        nodes_t nodes{make_http (options.node_a_url), make_http (options.node_b_url),
                      options.node_a_stream, options.node_b_stream};
        scenario_runner_t runner (nodes);
        for (const auto &name : selected_scenarios (options.scenario)) {
            if (name == "ST-A1") {
                runner.run_st_a1_scenario ();
            } else if (name == "ST-A2") {
                runner.run_st_a2_scenario ();
            } else if (name == "ST-A3") {
                runner.run_st_a3_scenario ();
            } else if (name == "ST-B1") {
                runner.run_st_b1_scenario ();
            } else if (name == "ST-B2") {
                runner.run_st_b2_scenario ();
            } else if (name == "ST-B3") {
                runner.run_st_b3_scenario ();
            } else if (name == "ST-B4") {
                runner.run_st_b4_scenario ();
            } else if (name == "ST-C1") {
                runner.run_st_c1_scenario ();
            } else if (name == "ST-C2") {
                runner.run_st_c2_scenario ();
            } else if (name == "ST-C3") {
                runner.run_st_c3_scenario ();
            } else if (name == "ST-D1") {
                runner.run_st_d1_scenario ();
            } else if (name == "ST-D2") {
                runner.run_st_d2_scenario ();
            } else if (name == "ST-E1") {
                runner.run_st_e1_scenario ();
            } else if (name == "ST-E2") {
                runner.run_st_e2_scenario ();
            } else if (name == "ST-F1") {
                runner.run_st_f1_scenario ();
            } else if (name == "ST-F2") {
                runner.run_st_f2_scenario ();
            } else if (name == "ST-F3") {
                runner.run_st_f3_scenario ();
            } else if (name == "ST-F4") {
                runner.run_st_f4_scenario ();
            } else if (name == "ST-F5") {
                runner.run_st_f5_scenario ();
            } else if (name == "ST-F6") {
                runner.run_st_f6_scenario ();
            } else {
                throw std::runtime_error ("Unknown scenario '" + name + "'.");
            }
            std::cout << "operation SpotActorTransfer." << name << " passed" << std::endl;
        }
        std::cout << "spot-actor-transfer e2e partial result=passed" << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "spot-actor-transfer e2e failed: " << error.what () << std::endl;
        return 1;
    }
}
