/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.9 and §10.10 preparation: one unbound Actor per logical stream, created through the public manager during setup
// (§4). The create is never retried; only the public RouteMesh status says when the Actor node is a ready peer.

#include <perf/server/actor_echo_support.hpp>
#include <perf/server/stream_loops.hpp>

namespace perf
{
// Returns the create result as evidence; the caller reports objectsReady once its probes have also finished.
inline json create_actors (role_t &role, const std::atomic<bool> &stopping)
{
    const auto &config = role.config;
    wait_for_public (role, stopping, [&] { return role.mesh.load ()->snapshot (*config.mesh_name).ready_peer_count > 0; },
                     "a ready Actor node peer");
    auto &manager = role.service<fw::actor_manager_t> ();
    std::mutex gate;
    std::uint64_t created = 0, existing = 0;
    std::int64_t total_ns = 0, max_ns = 0;
    for_each_concurrently (static_cast<int> (config.actor_ids.size ()), *config.workload.connect_concurrency, [&] (int index) {
        const auto &actor_id = config.actor_ids[static_cast<std::size_t> (index)];
        const auto started = now_ticks ();
        const auto result = manager.get_or_create (fw::actor_id_t (actor_id), perf_actor_type)
                              .in_mesh (*config.mesh_name)
                              .timeout (std::chrono::milliseconds (config.workload.setup_timeout_ms))
                              .async ()
                              .result ()
                              .value ();
        const auto elapsed = now_ticks () - started;
        std::lock_guard lock (gate);
        if (std::holds_alternative<fw::actor_create_created_t> (result))
            ++created;
        else if (std::holds_alternative<fw::actor_create_existing_t> (result))
            ++existing;
        else
            throw std::runtime_error ("Actor '" + actor_id + "' creation was rejected.");
        total_ns += elapsed;
        max_ns = std::max (max_ns, elapsed);
    });
    return {{"kind", "actorCreate"}, {"source", "actor_manager_t.get_or_create.in_mesh.async"},
            {"observedValue", {{"created", created}, {"existing", existing}, {"expectedActors", config.actor_ids.size ()},
                               {"createMeanMs", static_cast<double> (total_ns) / 1e6 / static_cast<double> (config.actor_ids.size ())},
                               {"createMaxMs", static_cast<double> (max_ns) / 1e6}}}};
}
} // namespace perf
