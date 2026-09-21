/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "supportchat_client_scenario.hpp"

#include <format>
#include <exception>
#include <iostream>
#include <string>

namespace
{

std::string read_option (int argc, char **argv, const std::string &name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return {};
}

} // namespace

int main (int argc, char **argv)
{
    const auto stream_endpoint = read_option (argc, argv, "--stream-endpoint");
    if (stream_endpoint.empty ()) {
        const std::string line = std::format ("usage: {} --stream-endpoint <endpoint>\n", argv[0]);
        std::cerr << line;
        return 2;
    }
    try {
        zlink::samples::supportchat::supportchat_client_scenario_t scenario;
        scenario.run (stream_endpoint);
        return 0;
    }
    catch (const std::exception &error) {
        const std::string line = std::format ("supportchat client failed: {}\n", error.what ());
        std::cerr << line;
        return 1;
    }
}
