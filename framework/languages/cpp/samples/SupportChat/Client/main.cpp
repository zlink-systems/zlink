/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "supportchat_client_scenario.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace
{

/* Standalone client는 직접 연결하는 endpoint만 CLI option으로 받는다(공통 정책 §4). */
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
        std::cerr << "usage: " << argv[0] << " --stream-endpoint <endpoint>\n";
        return 2;
    }
    try {
        zlink::samples::supportchat::supportchat_client_scenario_t scenario;
        scenario.run (stream_endpoint);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "supportchat client failed: " << error.what () << std::endl;
        return 1;
    }
}
