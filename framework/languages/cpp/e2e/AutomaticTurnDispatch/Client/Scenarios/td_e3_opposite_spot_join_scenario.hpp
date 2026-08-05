/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/user_spot_join_support.hpp"
#include "await_actor_scenario_context.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void run_td_e3_opposite_spot_join_scenario (
  TConnector &connector,
  const await_actor_scenario_context_t &actors)
{
    const auto spot_a = unique_id ("td-e3-a");
    const auto spot_b = unique_id ("td-e3-b");
    ensure_user_spot (connector, spot_a);
    ensure_user_spot (connector, spot_b);
    bind_actor_to_user_spot (connector, actors.actor_a, spot_a);
    bind_actor_to_user_spot (connector, actors.actor_b, spot_b);

    const auto run_id = unique_id ("TD-E3");
    const auto request_a = run_id + "-A";
    const auto request_b = run_id + "-B";
    using result_type = zlink::stream_connector::result_t<actor_await_res_t>;
    std::promise<result_type> promise_a;
    std::promise<result_type> promise_b;
    auto moved_a = promise_a.get_future ();
    auto moved_b = promise_b.get_future ();
    connector.request (actor_join_spot_req_t{.request_id = request_a,
                                              .target_spot_id = spot_b,
                                              .admission_delay_ms = 250})
      .packet_name (actor_join_spot_req_t::packet_name)
      .metadata (actor_id_metadata, actors.actor_a)
      .timeout (std::chrono::milliseconds (15000))
      .template submit<actor_await_res_t> (
        [&promise_a] (result_type result) { promise_a.set_value (std::move (result)); });
    connector.request (actor_join_spot_req_t{.request_id = request_b,
                                              .target_spot_id = spot_a,
                                              .admission_delay_ms = 250})
      .packet_name (actor_join_spot_req_t::packet_name)
      .metadata (actor_id_metadata, actors.actor_b)
      .timeout (std::chrono::milliseconds (15000))
      .template submit<actor_await_res_t> (
        [&promise_b] (result_type result) { promise_b.set_value (std::move (result)); });

    ensure (moved_a.wait_for (std::chrono::seconds (15)) == std::future_status::ready
              && moved_b.wait_for (std::chrono::seconds (15)) == std::future_status::ready,
            "TD-E3 opposite user Spot joins timed out");
    const auto result_a = moved_a.get ();
    const auto result_b = moved_b.get ();
    ensure_result (result_a, "TD-E3 actor A join failed");
    ensure_result (result_b, "TD-E3 actor B join failed");
    ensure (result_a.value ().marker == "actor-join-completed"
              && result_b.value ().marker == "actor-join-completed",
            "TD-E3 opposite user Spot join marker mismatch");
    assert_user_spot_join_evidence (connector, request_a);
    assert_user_spot_join_evidence (connector, request_b);
    assert_opposite_join_overlap (connector, run_id);
    std::cout << "scenario TD-E3 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
