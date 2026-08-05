/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/user_spot_join_support.hpp"
#include "await_actor_scenario_context.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void run_td_e2_user_to_user_spot_join_scenario (
  TConnector &connector,
  const await_actor_scenario_context_t &actors)
{
    bind_actor_to_user_spot (connector, actors.actor_a, actors.spot_id);
    const auto target_spot_id = unique_id ("td-e2-target");
    ensure_user_spot (connector, target_spot_id);
    const auto request_id = unique_id ("TD-E2");
    auto joined = connector.request (
                            actor_join_spot_req_t{.request_id = request_id,
                                                  .target_spot_id = target_spot_id,
                                                  .admission_delay_ms = 0})
                    .packet_name (actor_join_spot_req_t::packet_name)
                    .metadata (actor_id_metadata, actors.actor_a)
                    .timeout (std::chrono::milliseconds (15000))
                    .template submit<actor_await_res_t> ();
    ensure_result (joined, "TD-E2 user-to-user Spot join failed");
    ensure (joined.value ().marker == "actor-join-completed",
            "TD-E2 user-to-user Spot join marker mismatch");
    assert_user_spot_join_evidence (connector, request_id);
    std::cout << "scenario TD-E2 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
