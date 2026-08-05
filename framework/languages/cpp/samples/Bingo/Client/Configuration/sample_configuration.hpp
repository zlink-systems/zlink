/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <string>

namespace zlink::samples::bingo
{

inline sample_topology_t load_sample_topology (int argc, char **argv)
{
    sample_topology_t topology;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? std::string{} : argv[index];
        const std::string prefix = "--stream-endpoint=";
        if (arg.rfind (prefix, 0) == 0) {
            topology.stream_endpoint = arg.substr (prefix.size ());
            topology.session_a_stream_endpoint = topology.stream_endpoint;
            topology.session_b_stream_endpoint = topology.stream_endpoint;
        }
        else if (arg == "--stream-endpoint" && index + 1 < argc) {
            topology.stream_endpoint = argv[++index];
            topology.session_a_stream_endpoint = topology.stream_endpoint;
            topology.session_b_stream_endpoint = topology.stream_endpoint;
        }
        else if (arg.rfind ("--session-a-stream-endpoint=", 0) == 0) {
            topology.session_a_stream_endpoint = arg.substr (28);
        }
        else if (arg == "--session-a-stream-endpoint" && index + 1 < argc) {
            topology.session_a_stream_endpoint = argv[++index];
        }
        else if (arg.rfind ("--session-b-stream-endpoint=", 0) == 0) {
            topology.session_b_stream_endpoint = arg.substr (28);
        }
        else if (arg == "--session-b-stream-endpoint" && index + 1 < argc) {
            topology.session_b_stream_endpoint = argv[++index];
        }
    }
    return topology;
}

} // namespace zlink::samples::bingo
