/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-B1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_b1_scenario ()
{
    const auto actor_id = "actor-remote-ok-" + unique_suffix ();
    const auto spot_id = "spot-remote-ok-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 21);

    const auto join = join_actor (_nodes.a, actor_id, {"ST-B1", spot_id});
    require (join.accepted, "ST-B1 join was rejected.");

    const auto probe = probe_actor (_nodes.a, actor_id, {"ST-B1", "after-transfer"});
    require (probe.node_rid == "actor-b", "ST-B1 probe expected actor-b, got " + probe.node_rid);
    require (probe.state_version == 21,
             "ST-B1 state version expected 21, got " + std::to_string (probe.state_version));

    assert_evidence_sequence (_nodes.a, {"transfer|" + actor_id + "|transfer_out|21",
                                         "transfer|" + actor_id + "|leave|21",
                                         "message_flow|" + actor_id + "|commit_request|",
                                         "message_flow|" + actor_id + "|commit_ack|"});
    wait_evidence (_nodes.a, {"ST-B1|" + actor_id + "|success_reply|" + spot_id});
    assert_evidence_sequence (_nodes.b, {"ST-B1|" + actor_id + "|admission|",
                                         "transfer|" + actor_id + "|transfer_in|21",
                                         "message_flow|" + actor_id + "|location_committed|",
                                         "transfer|" + actor_id + "|joined|" + spot_id + ":21",
                                         "ST-B1|" + actor_id + "|packet_handler|after-transfer"});
    wait_evidence (_nodes.a, {"message_flow|" + actor_id + "|source_cleanup|"});

    assert_correlated_transfer_markers (
      {&_nodes.a, &_nodes.b}, actor_id,
      {"commit_request", "location_committed", "commit_ack", "source_cleanup"});
}

} // namespace
