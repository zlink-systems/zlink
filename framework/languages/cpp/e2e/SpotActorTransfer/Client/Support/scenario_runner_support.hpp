/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "scenario_context.hpp"

namespace
{

inline scenario_runner_t::scenario_runner_t (nodes_t &nodes) : _nodes (nodes)
{
}

inline void scenario_runner_t::in_flight_request_correlation ()
{
    const auto actor_id = "actor-inflight-req-" + unique_suffix ();
    const auto spot_id = "spot-inflight-req-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 106);
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F6", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F6|" + actor_id + "|joined_wait|" + spot_id});
    auto request_task = std::async (std::launch::async, [&] {
        return probe_ref (_nodes.a, actor_id, old_ref, {"ST-F6", "correlated-reply"},
                          std::chrono::seconds (5));
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    release_joined_gate (_nodes.b, spot_id);

    require (join_task.get ().accepted, "ST-F6 correlation transfer was rejected.");
    const auto response = request_task.get ();
    require (response.succeeded && response.reply && response.reply->node_rid == "actor-b",
             "ST-F6 reply did not correlate to the original caller: " + response.error_kind);
    require (response.reply->marker == "correlated-reply",
             "ST-F6 correlated reply marker mismatch.");
    wait_evidence (_nodes.b, {"ST-F6|" + actor_id + "|packet_handler|correlated-reply"});
    assert_request_handoff_frame (_nodes.a, _nodes.b, actor_id, "correlated-reply");
}
inline void scenario_runner_t::in_flight_request_timeout ()
{
    const auto actor_id = "actor-inflight-req-timeout-" + unique_suffix ();
    const auto spot_id = "spot-inflight-req-timeout-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 107);
    const auto old_ref = get_actor_ref (_nodes.a, actor_id);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-F6", spot_id}); });
    wait_evidence (_nodes.b, {"ST-F6|" + actor_id + "|joined_wait|" + spot_id});
    auto request_task = std::async (std::launch::async, [&] {
        return probe_ref (_nodes.a, actor_id, old_ref, {"ST-F6", "late-reply"},
                          std::chrono::milliseconds (250));
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (400));
    const auto timeout = request_task.get ();
    require (!timeout.succeeded && timeout.error_kind == "TimeoutException",
             "ST-F6 expected normal TimeoutException, got '" + timeout.error_kind + "'.");
    release_joined_gate (_nodes.b, spot_id);
    require (join_task.get ().accepted, "ST-F6 timeout transfer was rejected.");
    wait_evidence (_nodes.b, {"ST-F6|" + actor_id + "|packet_handler|late-reply",
                              "ST-F6|" + actor_id + "|late_reply_created|late-reply"});
    assert_request_handoff_frame (_nodes.a, _nodes.b, actor_id, "late-reply");
}
inline message_follow_setup_t scenario_runner_t::relocate_for_message_follow (
  const std::string &scenario,
  int state_version)
{
    const auto actor_id = "actor-message-follow-" + scenario + "-" + unique_suffix ();
    const auto spot_id = "spot-message-follow-" + scenario + "-" + unique_suffix ();
    const auto spot = create_spot_until_placed_on (_nodes.b, spot_id, "actor-b");
    const auto actor = create_actor_until_placed_on (
      _nodes.a, actor_id, e2e::actor_type_stateful, state_version, "actor-a");
    const auto old_ref = get_actor_ref (_nodes.a, actor.actor_id);
    require (join_actor (_nodes.a, actor.actor_id,
                         {scenario, spot.spot_id}).accepted,
             scenario + " transfer was rejected.");
    return {actor.actor_id, old_ref};
}
inline void scenario_runner_t::transfer_out_failure ()
{
    const auto actor_id = "actor-fail-transfer-out-" + unique_suffix ();
    const auto spot_id = "spot-fail-transfer-out-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_fail_transfer_out, 71);

    const auto response = join_actor (_nodes.a, actor_id, {"ST-C3", spot_id});
    require (!response.accepted, "ST-C3 transfer-out failure should not return accepted.");
    const auto source_evidence =
      wait_evidence (_nodes.a, {"ST-C3|" + actor_id + "|transfer_out_failed|71"});
    wait_evidence (_nodes.a, {"ST-C3|" + actor_id + "|join_failed|"});
    require_no_contains (source_evidence, "transfer|" + actor_id + "|leave|71",
                         "ST-C3 transfer-out failure should not leave source.");
    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-C3 transfer-out failure should not join target.");
}
inline void scenario_runner_t::source_leave_failure ()
{
    const auto actor_id = "actor-fail-leave-" + unique_suffix ();
    const auto spot_id = "spot-fail-leave-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_fail_leave, 72);

    const auto response = join_actor (_nodes.a, actor_id, {"ST-C3", spot_id});
    require (!response.accepted, "ST-C3 source leave failure should not return accepted.");
    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|transfer_out|72",
                               "ST-C3|" + actor_id + "|leave_failed|72",
                             });
    wait_evidence (_nodes.a, {"ST-C3|" + actor_id + "|join_failed|"});
    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence, "transfer|" + actor_id + "|transfer_in|72",
                         "ST-C3 source leave failure should not transfer in target.");
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-C3 source leave failure should not join target.");
}
inline void scenario_runner_t::transfer_in_failure ()
{
    const auto actor_id = "actor-fail-transfer-in-" + unique_suffix ();
    const auto spot_id = "spot-fail-transfer-in-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_fail_transfer_in, 73);

    const auto response = join_actor (_nodes.a, actor_id, {"ST-C3", spot_id});
    require (!response.accepted, "ST-C3 transfer-in failure should not return accepted.");
    wait_evidence (_nodes.b, {"ST-C3|" + actor_id + "|transfer_in_failed|73"});
    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|transfer_out|73",
                               "transfer|" + actor_id + "|leave|73",
                             });
    wait_evidence (_nodes.a, {"ST-C3|" + actor_id + "|join_failed|"});
    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-C3 transfer-in failure should not join target.");
}
inline void scenario_runner_t::joined_failure ()
{
    const auto actor_id = "actor-fail-joined-" + unique_suffix ();
    const auto spot_id = "spot-fail-joined-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "fail-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 74);

    const auto response = join_actor (_nodes.a, actor_id, {"ST-C3", spot_id});
    require (!response.accepted, "ST-C3 joined failure should not return accepted.");
    wait_evidence (_nodes.b, {"ST-C3|" + actor_id + "|joined_failed|" + spot_id});
    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|transfer_out|74",
                               "transfer|" + actor_id + "|leave|74",
                             });
    wait_evidence (_nodes.a, {"ST-C3|" + actor_id + "|join_failed|"});
    bool packet_failed = false;
    try {
        (void) probe_actor (_nodes.a, actor_id, {"ST-C3", "after-joined-failure"});
    }
    catch (const std::exception &) {
        packet_failed = true;
    }
    require (packet_failed, "ST-C3 actor packet succeeded after the joined callback failed.");
    const auto target_evidence = get_evidence (_nodes.b);
    require_no_contains (target_evidence,
                         "ST-C3|" + actor_id + "|packet_handler|after-joined-failure",
                         "ST-C3 joined failure should not dispatch as joined.");
}
inline void scenario_runner_t::local_location_commit_timing ()
{
    const auto actor_id = "actor-location-local-" + unique_suffix ();
    const auto spot_id = "spot-location-local-" + unique_suffix ();
    create_spot (_nodes.a, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 51);
    const auto before = get_actor_ref (_nodes.a, actor_id);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-D1", spot_id}); });
    const auto waiting =
      wait_evidence (_nodes.a, {
                                 "ST-D1|" + actor_id + "|admission|spot=" + spot_id,
                                 "ST-D1|" + actor_id + "|joined_wait|" + spot_id,
                               });
    require_no_contains (get_evidence (_nodes.a),
                         "ST-D1|" + actor_id + "|success_reply|" + spot_id,
                         "ST-D1 local join returned success before on_actor_joined completed.");
    const auto during = get_actor_ref (_nodes.a, actor_id);
    require (during.generation == before.generation,
             "ST-D1 local actor generation changed before joined completed.");

    auto blocked_probe = std::async (std::launch::async, [&] {
        return probe_actor (_nodes.a, actor_id, {"ST-D1", "during-joined-wait"});
    });
    require (blocked_probe.wait_for (std::chrono::milliseconds (500))
               == std::future_status::timeout,
             "ST-D1 actor packet completed before the local location commit.");
    require_no_contains (get_evidence (_nodes.a),
                         "ST-D1|" + actor_id + "|packet_handler|during-joined-wait",
                         "ST-D1 actor packet reached the target before commit.");

    release_joined_gate (_nodes.a, spot_id);
    const auto join = join_task.get ();
    require (join.accepted, "ST-D1 local join was rejected.");
    const auto after = get_actor_ref (_nodes.a, actor_id);
    require (after.generation == before.generation,
             "ST-D1 local actor generation changed across membership commit.");
    const auto probe = blocked_probe.get ();
    require (probe.spot_id == spot_id,
             "ST-D1 delayed actor packet did not resume on the committed target spot.");

    wait_evidence (_nodes.a, {
                               "ST-D1|" + actor_id + "|joined_released|" + spot_id,
                               "transfer|" + actor_id + "|joined|" + spot_id + ":51",
                               "ST-D1|" + actor_id + "|packet_handler|during-joined-wait",
                             });
    wait_evidence (_nodes.a, {"ST-D1|" + actor_id + "|success_reply|" + spot_id});
}
inline void scenario_runner_t::remote_location_commit_timing ()
{
    const auto actor_id = "actor-location-remote-" + unique_suffix ();
    const auto spot_id = "spot-location-remote-" + unique_suffix ();
    create_spot (_nodes.b, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 52);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-D1", spot_id}); });
    wait_evidence (_nodes.b, {
                               "ST-D1|" + actor_id + "|admission|spot=" + spot_id,
                               "ST-D1|" + actor_id + "|joined_wait|" + spot_id,
                             });
    const auto source_during = get_actor_ref (_nodes.a, actor_id);
    require (source_during.node_rid == "actor-a",
             "ST-D1 remote source ref moved before target joined completed. got="
               + source_during.node_rid);

    release_joined_gate (_nodes.b, spot_id);
    const auto join = join_task.get ();
    require (join.accepted, "ST-D1 remote join was rejected.");
    const auto target_after = get_actor_ref (_nodes.b, actor_id);
    require (target_after.node_rid == "actor-b",
             "ST-D1 remote target ref was not committed after joined completed. got="
               + target_after.node_rid);

    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|leave|52",
                             });
    wait_evidence (_nodes.a, {"ST-D1|" + actor_id + "|success_reply|" + spot_id});
    wait_evidence (_nodes.b, {
                               "ST-D1|" + actor_id + "|joined_released|" + spot_id,
                               "transfer|" + actor_id + "|joined|" + spot_id + ":52",
                             });
}

} // namespace
