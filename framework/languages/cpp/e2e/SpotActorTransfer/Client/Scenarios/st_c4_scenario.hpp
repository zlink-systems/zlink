/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-C4 injects the contract's exact-identity conflict on the canonical
 * relocation wire. The proxy forwards one relocationState(52) chunk and
 * immediately duplicates it with only the final chunk byte changed. The
 * relocation identity, attempt, coordinator fence, object, ordinal, and
 * encoded length are therefore identical while the checksum contribution is
 * different. The target must reject that staged conflict with
 * relocationFailed(53)/relocationDataLost(35), never restore the partial
 * payload, and the source Actor must remain usable with its original state. */
inline void scenario_runner_t::run_st_c4_scenario ()
{
    require (!_nodes.relocation_proxy_admin.empty (),
             "ST-C4 relocation chunk conflict proxy is not configured.");
    auto proxy = make_http (_nodes.relocation_proxy_admin);
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-chunk-conflict-" + unique_suffix (), "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-chunk-conflict-" + unique_suffix (),
      e2e::actor_type_stateful, 84, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    (void) proxy.post ("/arm").submit<nlohmann::json> ().result ().value ().body;
    const auto submitted = join_actor (_nodes.a, actor_id, {"ST-C4", spot_id});
    require (submitted.accepted,
             "ST-C4 faulted remote Join was not accepted for deferred execution.");

    /* Wait for the public terminal without assuming its kind yet, so a bad
     * mapping cannot prevent the wire and source-survival assertions from
     * preserving their own evidence. */
    const auto completion_marker =
      "deferred-join|" + actor_id + "|join_completion_failed|";
    const auto completion_evidence =
      _nodes.a.post ("/evidence/wait")
        .body (e2e::evidence_wait_req_t{{completion_marker}, 20000})
        .submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    require (evidence_contains (completion_evidence, completion_marker),
             "ST-C4 public Join failure terminal was not observed.");
    std::string public_failure_kind;
    for (const auto &entry : completion_evidence) {
        if (entry.scenario == "deferred-join" && entry.actor_id == actor_id
            && entry.kind == "join_completion_failed") {
            require (public_failure_kind.empty (),
                     "ST-C4 observed more than one public Join terminal.");
            public_failure_kind = entry.value;
        }
    }
    require (!public_failure_kind.empty (),
             "ST-C4 public Join failure terminal was not recorded.");

    const auto proxy_state =
      proxy.get ("/state").submit<nlohmann::json> ().result ().value ().body;
    require (proxy_state.at ("fired").get<bool> ()
               && proxy_state.at ("stateFrames").get<int> () >= 1
               && proxy_state.at ("injectedFrames").get<int> () == 1,
             "ST-C4 proxy did not inject exactly one conflicting command-52 chunk.");
    const auto failure_codes =
      proxy_state.at ("failureCodes").get<std::vector<int>> ();
    require (std::find (failure_codes.begin (), failure_codes.end (), 35)
               != failure_codes.end (),
             "ST-C4 target did not emit relocationFailed with relocationDataLost(35).");

    const auto source_probe =
      probe_actor (_nodes.a, actor_id, {"ST-C4", "source-survives-conflict"});
    require (source_probe.node_rid == "actor-a" && source_probe.state_version == 84
               && source_probe.marker == "source-survives-conflict",
             "ST-C4 source Actor did not survive with its original state.");

    const auto source_evidence = get_evidence (_nodes.a);
    require_no_contains (source_evidence, "transfer|" + actor_id + "|leave|",
                         "ST-C4 source left after target assembly conflict.");
    require_no_contains (source_evidence,
                         "message_flow|" + actor_id + "|source_cleanup|",
                         "ST-C4 source was cleaned up after target assembly conflict.");
    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence, "transfer|" + actor_id + "|transfer_in|",
                         "ST-C4 target restored a conflicting payload.");
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|",
                         "ST-C4 target ran OnJoinedActor after assembly conflict.");
    require_no_contains (
      target_evidence, "ST-C4|" + actor_id + "|packet_handler|source-survives-conflict",
      "ST-C4 source-survival probe reached the target.");

    /* framework_error_kind_t::data_lost is the public C++ terminal for the
     * wire's relocationDataLost(35). The Actor callback records its exact
     * enum value (11) in join_completion_failed evidence. */
    require (public_failure_kind == "11",
             "ST-C4 relocationDataLost(35) mapped to public FrameworkError:"
               + public_failure_kind + " instead of DataLost(11).");
}

} // namespace
