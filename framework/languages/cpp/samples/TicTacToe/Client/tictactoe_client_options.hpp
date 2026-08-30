/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Configuration/sample_names.hpp"
#include "Configuration/sample_topology.hpp"

#include <chrono>
#include <string>

namespace zlink::samples::tictactoe
{

struct tictactoe_client_options_t
{
    explicit tictactoe_client_options_t (const sample_topology_t &topology = sample_topology_t{})
    {
        api_http_endpoint = topology.api_http_endpoint;
    }

    std::string api_http_endpoint;
    std::string lifecycle_completion_file;
    std::string game_name = "tictactoe-game";
    std::string x_actor_id = sample_names_t::x_actor_id;
    std::string o_actor_id = sample_names_t::o_actor_id;
    std::string observer_actor_id = sample_names_t::observer_actor_id;
    std::chrono::milliseconds stream_timeout{5000};
};

} // namespace zlink::samples::tictactoe
