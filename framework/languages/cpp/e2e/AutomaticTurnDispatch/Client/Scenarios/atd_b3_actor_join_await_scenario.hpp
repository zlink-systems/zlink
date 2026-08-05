/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "../Support/scenario_assert.hpp"
#include "await_actor_scenario_context.hpp"

#include <zlink/stream_connector.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
std::string run_atd_b3_actor_join_await_scenario (
  TConnector &connector,
  const await_actor_scenario_context_t &actors)
{
    const auto request_id = unique_id ("ATD-B3");
    std::promise<zlink::stream_connector::result_t<actor_await_res_t>>
      actor_join_promise;
    auto actor_join = actor_join_promise.get_future ();
    connector.request (actor_join_await_req_t{.request_id = request_id,
                                              .target_node_rid = "play-a"})
      .packet_name (actor_join_await_req_t::packet_name)
      .metadata (actor_id_metadata, actors.actor_a)
      .timeout (std::chrono::milliseconds (30000))
      .template submit<actor_await_res_t> (
        [&] (zlink::stream_connector::result_t<actor_await_res_t> result) {
            actor_join_promise.set_value (std::move (result));
        });
    std::this_thread::sleep_for (std::chrono::milliseconds (75));
    auto actor_fast =
      connector.request (actor_fast_req_t{.request_id = request_id, .marker = "b3-fast"})
        .packet_name (actor_fast_req_t::packet_name)
        .metadata (actor_id_metadata, actors.actor_b)
        .timeout (std::chrono::milliseconds (30000))
        .template submit<actor_await_res_t> ();
    ensure (static_cast<bool> (actor_fast), "ATD-B3 ActorFastReq failed");
    auto actor_join_reply = actor_join.get ();
    ensure (static_cast<bool> (actor_join_reply), "ATD-B3 ActorJoinYieldReq failed");
    auto evidence =
      connector.request (await_evidence_req_t{.request_id = request_id})
        .packet_name (await_evidence_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-B3 evidence request failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"actor-join-await-started",
                                "actor-join-await-released",
                                "actor-fast-started",
                                "actor-fast-completed",
                                "actor-join-await-resumed",
                                "actor-join-await-completed"}),
            "ATD-B3 marker order mismatch");
    std::cout << "scenario ATD-B3 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
