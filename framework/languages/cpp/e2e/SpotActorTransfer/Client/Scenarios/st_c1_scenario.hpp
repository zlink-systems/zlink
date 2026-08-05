/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

/* ST-C1: this file owns the scenario orchestration and its public assertions. */
inline void scenario_runner_t::run_st_c1_scenario ()
{
    const auto actor_id = "actor-source-down-before-commit-" + unique_suffix ();
    const auto spot_id = "spot-source-down-before-commit-" + unique_suffix ();
    create_spot (_nodes.b, spot_id);
    create_actor (_nodes.a, actor_id, e2e::actor_type_stateful, 62);

    auto join_task =
      std::async (std::launch::async, [&] () -> std::optional<e2e::join_target_res_t> {
          try {
              return join_actor (_nodes.a, actor_id, {"ST-C1", spot_id});
          }
          catch (const std::exception &) {
              // Source shutdown may abort the HTTP request before the
              // endpoint can return a failure body.
              return std::nullopt;
          }
      });
    wait_evidence (_nodes.b, {"ST-C1|" + actor_id + "|admission|spot=" + spot_id});
    wait_evidence (_nodes.a, {
                               "transfer|" + actor_id + "|transfer_out|62",
                               "ST-C1|" + actor_id + "|before_commit_gate|62",
                             });

    shutdown_node (_nodes.a);
    if (join_task.wait_for (std::chrono::seconds (3)) == std::future_status::ready) {
        const auto response = join_task.get ();
        require (!response || !response->accepted,
                 "ST-C1 join should not be accepted after source shutdown before commit.");
    }

    const auto target_evidence =
      wait_evidence (_nodes.b, {"message_flow|" + actor_id + "|pending_admission_expired|"});
    require_no_contains (target_evidence, "transfer|" + actor_id + "|transfer_in|62",
                         "ST-C1 target should not transfer in without commit.");
    require_no_contains (target_evidence, "transfer|" + actor_id + "|joined|" + spot_id,
                         "ST-C1 target should not join without commit.");
    require_no_contains (target_evidence, "ST-C1|" + actor_id + "|packet_handler|",
                         "ST-C1 target should not dispatch actor packets without commit.");
}

} // namespace
