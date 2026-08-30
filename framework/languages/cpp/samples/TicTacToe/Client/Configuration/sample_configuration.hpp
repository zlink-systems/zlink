/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <string>

namespace zlink::samples::tictactoe
{

/* Standalone client의 application 입력은 직접 붙는 endpoint뿐이다. Runner가 lifecycle evidence 뒤
 * connector close를 허용하는 실행 제어 file은 optional CLI option으로 받으며 환경 변수는 읽지 않는다
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

inline std::string load_lifecycle_completion_file (int argc, char **argv)
{
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? std::string{} : argv[index];
        const std::string prefix = "--lifecycle-completion-file=";
        if (arg.rfind (prefix, 0) == 0) {
            return arg.substr (prefix.size ());
        }
        if (arg == "--lifecycle-completion-file" && index + 1 < argc) {
            return argv[index + 1];
        }
    }
    return {};
}

} // namespace zlink::samples::tictactoe
