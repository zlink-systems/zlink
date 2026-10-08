/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Actor Object Server hosts the Actors that a remote Session binds (§10.2) or an ActorCaller addresses by
// ActorId (§10.9, §10.10).

#include <perf/server/actor_echo_support.hpp>

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.role != "actor"
        || (config.scenario != "cs-remote-session-actor-echo"
            && config.scenario != "actor-no-bind-request-echo"
            && config.scenario != "actor-no-bind-send-send-echo"))
        throw std::invalid_argument ("ActorServer runs the actor role of §10.2, §10.9 and §10.10.");
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    auto &role_ref = *role;
    const auto &settings = role->config;
    return perf::run_role (
      std::move (role), [&] (fw::zlink_framework_options_t &options, fw::app_t &app) {
          auto mesh = options.add_route_mesh (*settings.mesh_name);
          mesh.set_automatic_routing_id_prefix ("perf-actor");
          mesh.listen (settings.transport_endpoints.at ("mesh"));
          // §10.10: the Actor answers through the public Channel client; the caller is the return channel Server.
          if (settings.mode == "send-send")
              mesh.channel (*settings.channel_name).client ();
          perf::add_perf_actors (mesh);
          app.add_hosted_service (std::make_unique<perf::actor_placement_watcher_t> (role_ref));
      });
}
