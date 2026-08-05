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
std::string run_atd_b2_same_actor_reentry_scenario (
  TConnector &connector,
  TConnector &,
  const await_actor_scenario_context_t &actors)
{
    const auto request_id = unique_id ("ATD-B2");
    std::promise<zlink::stream_connector::result_t<actor_await_res_t>> actor_await_promise;
    auto actor_yield = actor_await_promise.get_future ();
    connector.request (actor_await_req_t{.request_id = request_id, .delay_ms = 350})
      .packet_name (actor_await_req_t::packet_name)
      .metadata (actor_id_metadata, actors.actor_a)
      .timeout (std::chrono::milliseconds (30000))
      .template submit<actor_await_res_t> (
        [&] (zlink::stream_connector::result_t<actor_await_res_t> result) {
            actor_await_promise.set_value (std::move (result));
        });
    std::this_thread::sleep_for (std::chrono::milliseconds (75));
    auto actor_fast =
      connector.request (actor_fast_req_t{.request_id = request_id, .marker = "b2-fast"})
        .packet_name (actor_fast_req_t::packet_name)
        .metadata (actor_id_metadata, actors.actor_a)
        .timeout (std::chrono::milliseconds (30000))
        .template submit<actor_await_res_t> ();
    ensure (static_cast<bool> (actor_fast), "ATD-B2 ActorFastReq failed");
    auto actor_await_reply = actor_yield.get ();
    ensure (static_cast<bool> (actor_await_reply), "ATD-B2 ActorYieldReq failed");
    auto evidence =
      connector.request (await_evidence_req_t{.request_id = request_id})
        .packet_name (await_evidence_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (evidence), "ATD-B2 evidence request failed");
    ensure (contains_in_order (evidence.value ().evidence, request_id,
                               {"actor-await-started", "actor-await-released",
                                "actor-await-resumed", "actor-await-completed",
                                "actor-fast-started", "actor-fast-completed"}),
            "ATD-B2 marker order mismatch");
    std::cout << "scenario ATD-B2 passed\n";
    return request_id;
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
