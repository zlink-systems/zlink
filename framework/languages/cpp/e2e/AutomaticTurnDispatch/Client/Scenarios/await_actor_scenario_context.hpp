/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

struct await_actor_scenario_context_t
{
    std::string spot_id;
    std::string actor_a;
    std::string actor_b;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
