/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <string>

namespace zlink::samples::tictactoe
{

/* Standalone client는 직접 붙는 endpoint만 CLI option으로 받는다. 환경 변수는 읽지 않는다
 * (공통 정책 sample-e2e-configuration-policy.ko.md §4). */
inline sample_topology_t load_sample_topology (int argc, char **argv)
{
    sample_topology_t topology;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? std::string{} : argv[index];
        const std::string prefix = "--api-http-endpoint=";
        if (arg.rfind (prefix, 0) == 0) {
            topology.api_http_endpoint = arg.substr (prefix.size ());
        }
        else if (arg == "--api-http-endpoint" && index + 1 < argc) {
            topology.api_http_endpoint = argv[++index];
        }
    }
    return topology;
}

} // namespace zlink::samples::tictactoe
