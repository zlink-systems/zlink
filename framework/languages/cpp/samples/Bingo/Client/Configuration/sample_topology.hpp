/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::samples::bingo
{

struct sample_topology_t
{
    std::string stream_endpoint = "tcp://127.0.0.1:47114";
    std::string session_a_stream_endpoint = "tcp://127.0.0.1:47114";
    std::string session_b_stream_endpoint = "tcp://127.0.0.1:47117";
};

} // namespace zlink::samples::bingo
