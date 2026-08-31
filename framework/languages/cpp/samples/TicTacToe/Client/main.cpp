/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "tictactoe_client_scenario.hpp"

#include "Configuration/sample_configuration.hpp"

#include <iostream>
#include <stdexcept>

int main (int argc, char **argv)
{
    using namespace zlink::samples::tictactoe;

    try {
        tictactoe_client_options_t options{load_sample_topology (argc, argv)};
        options.lifecycle_completion_file = load_lifecycle_completion_file (argc, argv);
        if (!tictactoe_client_scenario_t{}.run (options)) { return 1; }
        std::cout << "tictactoe=completed\n";
        return 0;
    }
    catch (const std::exception &ex) {
        std::cerr << "tictactoe=failed " << ex.what () << '\n';
        return 1;
    }
}
