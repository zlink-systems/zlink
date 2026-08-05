/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b3_scenario ()
{
    const auto actor_id = "actor-no-adapter-" + unique_suffix ();
    const auto spot_id = "spot-no-adapter-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_no_adapter, 31);

    const auto join = join_actor (_nodes.a, actor_id, {"ST-B3", spot_id});
    require (join.accepted, "ST-B3 join was rejected.");

    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-B3", "after-default-empty-transfer"});
    require (probe.node_rid == "actor-b", "ST-B3 probe expected actor-b, got " + probe.node_rid);
    require (probe.state_version == 0, "ST-B3 default empty target state expected 0, got "
                                         + std::to_string (probe.state_version));
    assert_evidence_sequence (_nodes.a,
                              {"transfer|" + actor_id + "|transfer_out_empty_default|no-adapter",
                               "transfer|" + actor_id + "|leave|31",
                               "message_flow|" + actor_id + "|commit_request|",
                               "message_flow|" + actor_id + "|commit_ack|"});
    wait_evidence (_nodes.a, {"ST-B3|" + actor_id + "|success_reply|" + spot_id});
    assert_evidence_sequence (
      _nodes.b, {"ST-B3|" + actor_id + "|admission|",
                 "transfer|" + actor_id + "|transfer_in_empty_default|actor-factory",
                 "transfer|" + actor_id + "|joined|" + spot_id + ":0",
                 "message_flow|" + actor_id + "|location_committed|",
                 "ST-B3|" + actor_id + "|packet_handler|after-default-empty-transfer"});
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});
    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"commit_request", "location_committed", "commit_ack", "source_cleanup"});
}

} // namespace
