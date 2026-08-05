/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-A3: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_a3_scenario ()
{
    const auto actor_id = "actor-local-moving-" + unique_suffix ();
    const auto spot_id = "spot-local-moving-" + unique_suffix ();
    create_spot (_nodes.a, spot_id, "delay-joined");
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 13);

    auto join_task = std::async (
      std::launch::async, [&] { return join_actor (_nodes.a, actor_id, {"ST-A3", spot_id}); });
    const auto waiting =
      wait_evidence (_nodes.a, {
                                 "ST-A3|" + actor_id + "|admission|spot=" + spot_id,
                                 "transfer|" + actor_id + "|leave|13",
                                 "ST-A3|" + actor_id + "|joined_wait|" + spot_id,
                               });
    require_no_contains (waiting, "ST-A3|" + actor_id + "|packet_handler|during-joined-wait",
                         "ST-A3 packet should not run before on_actor_joined is released.");

    auto blocked_probe = std::async (std::launch::async, [&] {
        return probe_actor (_nodes.a, actor_id, {"ST-A3", "during-joined-wait"});
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    require (blocked_probe.wait_for (std::chrono::seconds (0)) != std::future_status::ready,
             "ST-A3 actor packet completed while on_actor_joined was still blocked.");

    const auto release = release_joined_gate (_nodes.a, spot_id);
    require (release.released, "ST-A3 joined gate was already released.");

    const auto join = join_task.get ();
    require (join.accepted, "ST-A3 join was rejected.");
    const auto probe = blocked_probe.get ();
    require (probe.spot_id == spot_id, "ST-A3 blocked packet did not resume on the target spot.");

    wait_evidence (_nodes.a, {
                               "ST-A3|" + actor_id + "|joined_released|" + spot_id,
                               "transfer|" + actor_id + "|joined|" + spot_id + ":13",
                               "ST-A3|" + actor_id + "|packet_handler|during-joined-wait",
                             });
    wait_evidence (_nodes.a, {"ST-A3|" + actor_id + "|success_reply|" + spot_id});
}

} // namespace
