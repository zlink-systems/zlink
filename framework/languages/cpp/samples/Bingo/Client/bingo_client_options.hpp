/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Configuration/sample_topology.hpp"

#include <chrono>
#include <string>

namespace zlink::samples::bingo
{

struct bingo_client_options_t
{
    explicit bingo_client_options_t (const sample_topology_t &topology = sample_topology_t{})
    {
        stream_endpoint = topology.stream_endpoint;
        session_a_stream_endpoint = topology.session_a_stream_endpoint;
        session_b_stream_endpoint = topology.session_b_stream_endpoint;
    }

    std::string stream_endpoint;
    std::string session_a_stream_endpoint;
    std::string session_b_stream_endpoint;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{5000};
};

} // namespace zlink::samples::bingo
