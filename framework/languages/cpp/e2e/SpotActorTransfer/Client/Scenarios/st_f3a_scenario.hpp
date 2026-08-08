/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_runner_support.hpp"

namespace
{

inline void scenario_runner_t::run_st_f3a_scenario ()
{
    require (_nodes.c.has_value (), "ST-F3A actor-c is not configured.");
    require (!_nodes.session_proxy_admin.empty (),
             "ST-F3A Session route proxy is not configured.");
    auto proxy = make_http (_nodes.session_proxy_admin);
    const auto actor = create_actor_until_placed_on (
      _nodes.a, "actor-late-session-route-" + unique_suffix (),
      e2e::actor_type_stateful, 1031, "actor-a");
    const auto spot_b = create_spot_until_placed_on (
      _nodes.b, "spot-late-session-b-" + unique_suffix (), "actor-b");
    const auto spot_c = create_spot_until_placed_on (
      *_nodes.c, "spot-late-session-c-" + unique_suffix (), "actor-c");
    const auto source_ref = get_actor_ref (_nodes.a, actor.actor_id);
    bound_session_t bound (_nodes.a_stream_endpoint, "ST-F3A", source_ref);

    (void) proxy.post ("/hold")
      .submit<nlohmann::json> ().result ().value ().body;
    require (join_actor (_nodes.a, actor.actor_id,
                         {"ST-F3A", spot_b.spot_id}).accepted,
             "ST-F3A A-to-B Join was rejected.");
    wait_evidence (
      _nodes.b, {"ST-F3A|" + actor.actor_id + "|join_completion_accepted|"});

    const auto held_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    std::uint64_t held_bytes = 0;
    while (std::chrono::steady_clock::now () < held_deadline) {
        const auto stats = proxy.get ("/stats")
                             .submit<nlohmann::json> ().result ().value ().body;
        held_bytes = stats.value ("heldBytes", std::uint64_t{0});
        if (held_bytes != 0)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    require (held_bytes != 0,
             "ST-F3A did not retain actor-b to Session owner bytes.");

    require (join_actor (_nodes.b, actor.actor_id,
                         {"ST-F3A", spot_c.spot_id}).accepted,
             "ST-F3A B-to-C Join was rejected.");
    const auto route_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    e2e::bound_actor_ref_res_t current;
    while (std::chrono::steady_clock::now () < route_deadline) {
        current = bound.actor_ref ();
        if (current.node_rid == "actor-c")
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    require (current.node_rid == "actor-c",
             "ST-F3A bound ActorRef did not advance to actor-c.");

    (void) proxy.post ("/release")
      .submit<nlohmann::json> ().result ().value ().body;
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    require (bound.actor_ref ().node_rid == "actor-c",
             "ST-F3A late actor-b route replaced actor-c.");

    const std::string marker = "push-from-c";
    auto notify = bound.expect_push (marker);
    const auto response = bound.bound_push ({"ST-F3A", marker});
    const auto pushed = notify.get ();
    require (response.node_rid == "actor-c" && pushed.node_rid == "actor-c",
             "ST-F3A bound push was not produced by actor-c.");
    require_no_contains (
      get_evidence (_nodes.b),
      "ST-F3A|" + actor.actor_id + "|bound_push|" + marker,
      "ST-F3A actor-b produced the current bound push.");
}

} // namespace
