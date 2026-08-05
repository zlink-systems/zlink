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

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void run_atd_c3_actor_timer_isolation_scenario (
  TConnector &connector,
  const await_actor_scenario_context_t &actors)
{
    const auto actor_then_timer = unique_id ("ATD-C3A");
    std::promise<zlink::stream_connector::result_t<actor_await_res_t>>
      actor_await_promise;
    auto actor_yield = actor_await_promise.get_future ();
    connector.request (actor_await_req_t{.request_id = actor_then_timer, .delay_ms = 1500})
      .packet_name (actor_await_req_t::packet_name)
      .metadata (actor_id_metadata, actors.actor_a)
      .timeout (std::chrono::milliseconds (30000))
      .template submit<actor_await_res_t> (
        [&] (zlink::stream_connector::result_t<actor_await_res_t> result) {
            actor_await_promise.set_value (std::move (result));
        });
    auto actor_released =
      connector.request (
                await_evidence_wait_req_t{.request_id = actor_then_timer,
                                          .marker = "actor-await-released",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (actor_released), "ATD-C3A actor-await-released wait failed");
    connector.send (timer_start_msg_t{.request_id = actor_then_timer,
                                      .timer_name = actor_then_timer + "-fast",
                                      .mode = "fast",
                                      .period_ms = 50,
                                      .delay_ms = 0})
        .packet_name (timer_start_msg_t::packet_name)
        .metadata (spot_id_metadata, actors.spot_id)
        .submit ();
    auto fast_timer_completed =
      connector.request (
                await_evidence_wait_req_t{.request_id = actor_then_timer,
                                          .marker = "timer-fast-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (fast_timer_completed),
            "ATD-C3A timer-fast-completed wait failed");
    connector.send (timer_stop_msg_t{.request_id = actor_then_timer})
        .packet_name (timer_stop_msg_t::packet_name)
        .metadata (spot_id_metadata, actors.spot_id)
        .submit ();
    auto actor_await_reply = actor_yield.get ();
    ensure (static_cast<bool> (actor_await_reply), "ATD-C3A ActorYieldReq failed");
    auto actor_timer_evidence =
      connector.request (await_evidence_req_t{.request_id = actor_then_timer})
        .packet_name (await_evidence_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (actor_timer_evidence),
            "ATD-C3A evidence request failed");
    ensure_contains_in_order (actor_timer_evidence.value ().evidence,
                              actor_then_timer,
                              {"actor-await-started",
                               "actor-await-released",
                               "timer-fast-started",
                               "timer-fast-completed",
                               "actor-await-resumed",
                               "actor-await-completed"},
                              "ATD-C3A marker order mismatch");

    const auto timer_then_actor = unique_id ("ATD-C3B");
    connector.send (timer_start_msg_t{.request_id = timer_then_actor,
                                      .timer_name = timer_then_actor + "-await",
                                      .mode = "await-on-first",
                                      .period_ms = 500,
                                      .delay_ms = 350})
        .packet_name (timer_start_msg_t::packet_name)
        .metadata (spot_id_metadata, actors.spot_id)
        .submit ();
    auto await_timer_released =
      connector.request (
                await_evidence_wait_req_t{.request_id = timer_then_actor,
                                          .marker = "timer-await-released",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (await_timer_released),
            "ATD-C3B timer-await-released wait failed");
    auto actor_fast =
      connector.request (actor_fast_req_t{.request_id = timer_then_actor,
                                          .marker = "c3-actor-fast"})
        .packet_name (actor_fast_req_t::packet_name)
        .metadata (actor_id_metadata, actors.actor_b)
        .timeout (std::chrono::milliseconds (30000))
        .template submit<actor_await_res_t> ();
    ensure (static_cast<bool> (actor_fast), "ATD-C3B ActorFastReq failed");
    auto timer_actor_evidence =
      connector.request (
                await_evidence_wait_req_t{.request_id = timer_then_actor,
                                          .marker = "timer-await-completed",
                                          .timeout_milliseconds = 3000})
        .packet_name (await_evidence_wait_req_t::packet_name)
        .metadata (target_node_rid_metadata, "play-a")
        .timeout (std::chrono::milliseconds (30000))
        .template submit<await_evidence_res_t> ();
    ensure (static_cast<bool> (timer_actor_evidence),
            "ATD-C3B evidence wait failed");
    connector.send (timer_stop_msg_t{.request_id = timer_then_actor})
        .packet_name (timer_stop_msg_t::packet_name)
        .metadata (spot_id_metadata, actors.spot_id)
        .submit ();
    ensure_contains_in_order (timer_actor_evidence.value ().evidence,
                              timer_then_actor,
                              {"timer-await-started",
                               "timer-await-released",
                               "actor-fast-started",
                               "actor-fast-completed",
                               "timer-await-resumed",
                               "timer-await-completed"},
                              "ATD-C3B marker order mismatch");
    std::cout << "scenario ATD-C3 passed\n";
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
