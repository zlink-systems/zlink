/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "gamequest_client_scenario.hpp"

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
    const auto api_a_stream = read_option (argc, argv, "--api-a-stream-endpoint");
    const auto api_b_stream = read_option (argc, argv, "--api-b-stream-endpoint");
    const auto api_a_http = read_option (argc, argv, "--api-a-http-url");
    const auto api_b_http = read_option (argc, argv, "--api-b-http-url");
    if (api_a_stream.empty () || api_b_stream.empty () || api_a_http.empty ()
        || api_b_http.empty ()) {
        std::cerr << "usage: " << argv[0]
                  << " --api-a-stream-endpoint <endpoint> --api-b-stream-endpoint <endpoint>"
                     " --api-a-http-url <url> --api-b-http-url <url>\n";
        return 2;
    }
    if (!zlink::samples::gamequest::gamequest_client_scenario_t{}.run (api_a_stream, api_b_stream,
                                                                      api_a_http, api_b_http)) {
        std::cerr << "gamequest=failed\n";
        return 1;
    }
    return 0;
}
