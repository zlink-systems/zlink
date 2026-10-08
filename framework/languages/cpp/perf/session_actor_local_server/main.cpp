/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// §10.1: the STREAM session and the Actors it binds live on this one Object Server node.

#include <perf/server/actor_echo_support.hpp>

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.scenario != "cs-local-session-actor-echo" || config.role != "session-actor-local")
        throw std::invalid_argument (
          "SessionActorLocalServer runs the cs-local-session-actor-echo role.");
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    const auto &settings = role->config;
    return perf::run_role (
      std::move (role), [&] (fw::zlink_framework_options_t &options, fw::app_t &) {
          auto mesh = options.add_route_mesh (*settings.mesh_name);
          mesh.set_automatic_routing_id_prefix ("perf-session-actor-local");
          mesh.listen (settings.transport_endpoints.at ("mesh"));
          perf::add_perf_actors (mesh);
          options.services ().add_singleton<perf::session_actor_setup_t, perf::role_t> ();
          options.add_stream_node ("perf-session")
            .bind (settings.transport_endpoints.at ("stream"))
            .enable_actor_dispatch ()
            .register_session<perf::perf_actor_relay_session_t> ();
      });
}
