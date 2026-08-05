/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/sf_a1_scenario.hpp"
#include "Scenarios/sf_a2_scenario.hpp"
#include "Scenarios/sf_b1_scenario.hpp"
#include "Scenarios/sf_b2_scenario.hpp"
#include "Scenarios/sf_c1_scenario.hpp"
#include "Scenarios/sf_c2_scenario.hpp"
#include "Scenarios/sf_d1_scenario.hpp"
#include "Scenarios/sf_d2_scenario.hpp"
#include "Scenarios/sf_d3_scenario.hpp"
#include "Scenarios/sf_e1_scenario.hpp"

int main (int argc, char **argv)
{
    const auto options = read_options (argc, argv);
    if (options.scenario == "SF-A1") {
        run_sf_a1_scenario (options);
    } else if (options.scenario == "SF-A2") {
        run_sf_a2_scenario (options);
    } else if (options.scenario == "SF-B1") {
        run_sf_b1_scenario (options);
    } else if (options.scenario == "SF-B2") {
        run_sf_b2_scenario (options);
    } else if (options.scenario == "SF-C1") {
        run_sf_c1_scenario (options);
    } else if (options.scenario == "SF-C2") {
        run_sf_c2_scenario (options);
    } else if (options.scenario == "SF-D1") {
        run_sf_d1_scenario (options);
    } else if (options.scenario == "SF-D2") {
        run_sf_d2_scenario (options);
    } else if (options.scenario == "SF-D3") {
        run_sf_d3_scenario (options);
    } else if (options.scenario == "SF-E1") {
        run_sf_e1_scenario (options);
    } else {
        throw std::runtime_error ("Unsupported StoreFailure scenario: " + options.scenario);
    }
    std::cout << "store-failure client scenario=" << options.scenario << " result=passed\n";
    return 0;
}
