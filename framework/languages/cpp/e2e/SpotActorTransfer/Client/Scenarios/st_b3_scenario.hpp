/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b3_scenario ()
{
    const auto spot = create_spot_until_placed_on (
      _nodes.b, "spot-no-adapter-" + unique_suffix (), "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-no-adapter-" + unique_suffix (),
      e2e::actor_type_no_adapter, 31, "actor-a");
    const auto &actor_id = actor.actor_id;
    const auto &spot_id = spot.spot_id;

    const auto join = join_actor (_nodes.a, actor_id, {"ST-B3", spot_id});
    require (join.accepted, "ST-B3 deferred Join was not submitted.");
    wait_evidence (
      _nodes.b, {"ST-B3|" + actor_id + "|join_completion_accepted|"});

    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-B3", "after-default-empty-transfer"});
    require (probe.node_rid == "actor-b", "ST-B3 probe expected actor-b, got " + probe.node_rid);
    require (probe.state_version == 0, "ST-B3 default empty target state expected 0, got "
                                         + std::to_string (probe.state_version));
    // The relocation commit is a single Location Store CAS, not a mesh
    // commit_request/commit_ack packet exchange (dead since 8bae89dc0f,
    // "align admission and relocation ownership") -- it is observed once,
    // on the target, as message_flow|<actor>|location_committed| (checked
    // below in the _nodes.b sequence, in order before joined).
    assert_evidence_sequence (_nodes.a,
                              {"transfer|" + actor_id + "|transfer_out_empty_default|no-adapter",
                               "transfer|" + actor_id + "|leave|31"});
    wait_evidence (_nodes.a, {"ST-B3|" + actor_id + "|success_reply|" + spot_id});
    assert_evidence_sequence (
      _nodes.b, {"ST-B3|" + actor_id + "|admission|",
                 "transfer|" + actor_id + "|transfer_in_empty_default|actor-factory",
                 "message_flow|" + actor_id + "|location_committed|",
                 "transfer|" + actor_id + "|joined|" + spot_id + ":0",
                 "ST-B3|" + actor_id + "|packet_handler|after-default-empty-transfer"});
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});
    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"location_committed", "source_cleanup"});
}

} // namespace
