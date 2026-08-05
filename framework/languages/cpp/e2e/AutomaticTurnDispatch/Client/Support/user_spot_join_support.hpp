/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/automatic_turn_dispatch_contracts.hpp"
#include "scenario_assert.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

template <typename TConnector>
void ensure_user_spot (TConnector &connector, const std::string &spot_id)
{
    auto ensured = connector.request (ensure_spot_req_t{.spot_id = spot_id})
                     .packet_name (ensure_spot_req_t::packet_name)
                     .timeout (std::chrono::milliseconds (15000))
                     .template submit<ensure_spot_res_t> ();
    ensure_result (ensured, "execution-turn ensure user Spot failed");
    ensure (ensured.value ().spot_id == spot_id,
            "execution-turn ensure user Spot reply mismatch");
}

template <typename TConnector>
void bind_actor_to_user_spot (TConnector &connector,
                              const std::string &actor_id,
                              const std::string &spot_id)
{
    auto bound = connector.request (
                            bind_await_actors_req_t{.spot_id = spot_id,
                                                    .actor_ids = {actor_id}})
                   .packet_name (bind_await_actors_req_t::packet_name)
                   .timeout (std::chrono::milliseconds (15000))
                   .template submit<bind_await_actors_res_t> ();
    ensure_result (bound, "execution-turn bind actor to user Spot failed");
    ensure (bound.value ().actors.size () == 1
              && bound.value ().actors.front ().actor_id == actor_id,
            "execution-turn actor binding reply mismatch");
}

inline std::string marker_turn (const std::vector<std::string> &evidence,
                                const std::string &request_id,
                                std::string_view marker)
{
    for (const auto &line : evidence) {
        if (line.find ("request=" + request_id) == std::string::npos
            || line.find (marker) == std::string::npos) {
            continue;
        }
        const auto begin = line.find ("|turn=");
        if (begin == std::string::npos) {
            return {};
        }
        const auto value_begin = begin + 6;
        const auto end = line.find ('|', value_begin);
        return line.substr (value_begin, end - value_begin);
    }
    return {};
}

template <typename TConnector>
void assert_user_spot_join_evidence (TConnector &connector,
                                     const std::string &request_id)
{
    auto evidence = connector.request (await_evidence_req_t{.request_id = request_id})
                      .packet_name (await_evidence_req_t::packet_name)
                      .metadata (target_node_rid_metadata, "play-a")
                      .timeout (std::chrono::milliseconds (15000))
                      .template submit<await_evidence_res_t> ();
    ensure_result (evidence, "execution-turn user Spot join evidence request failed");
    ensure_contains_in_order (
      evidence.value ().evidence, request_id,
      {"actor-join-target-completed", "actor-join-source-left",
       "actor-join-target-joined", "actor-join-completed"},
      "execution-turn user Spot join lifecycle order mismatch");

    const auto started = marker_turn (evidence.value ().evidence, request_id,
                                      "actor-join-started");
    const auto source_left = marker_turn (evidence.value ().evidence, request_id,
                                          "actor-join-source-left");
    const auto completed = marker_turn (evidence.value ().evidence, request_id,
                                        "actor-join-completed");
    ensure (!started.empty () && started != "0" && started == source_left
              && started == completed,
            "execution-turn source leave did not stay in the caller turn");
}

template <typename TConnector>
void assert_opposite_join_overlap (TConnector &connector, const std::string &run_id)
{
    auto evidence = connector.request (await_evidence_req_t{.request_id = run_id})
                      .packet_name (await_evidence_req_t::packet_name)
                      .metadata (target_node_rid_metadata, "play-a")
                      .timeout (std::chrono::milliseconds (15000))
                      .template submit<await_evidence_res_t> ();
    ensure_result (evidence, "TD-E3 overlap evidence request failed");
    std::size_t admissions_started = 0;
    bool completion_seen = false;
    for (const auto &line : evidence.value ().evidence) {
        if (line.find ("actor-join-target-started") != std::string::npos) {
            ensure (!completion_seen,
                    "TD-E3 target admissions did not overlap before the first completion");
            ++admissions_started;
        }
        if (line.find ("actor-join-target-completed") != std::string::npos) {
            completion_seen = true;
        }
    }
    ensure (admissions_started == 2 && completion_seen,
            "TD-E3 did not observe both concurrent target admissions");
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
