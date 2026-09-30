/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// What every Spot role of §10.3-§10.8 shares: an automatic RouteMesh Object Server hosting this cell's User Spots as
// Actor-less SpotWide Spots (§10), and their creation through the public manager as setup. No call that a scenario
// measures lives here; each scenario file shows its own requests, sends and Yield/worker calls.

#include <perf/server/actor_echo_support.hpp>
#include <perf/server/stream_loops.hpp>

namespace perf
{
inline constexpr const char *perf_spot_type = "perf-spot";

// The Actor-less Spot: `spot_t` needs an actor type by its declaration, but this Spot never admits an Actor.
template <typename TDerived> class perf_spot_base_t : public fw::spot_t<perf_actor_t>
{
  public:
    explicit perf_spot_base_t (fw::spot_context_t context) : _context (std::move (context)) {}
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view, const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::reject ();
    }
    fw::task_t<void> on_actor_joined (perf_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (perf_actor_t &) override { co_return; }

  protected:
    fw::spot_context_t _context;
};

// The automatic RouteMesh Object Server node that hosts the User Spots of this cell; `callsChannel` roles also register a
// Channel client (§10.5, §10.6). TSpot receives role_t and route_client_t as constructor dependencies.
template <typename TSpot>
void add_spot_role (fw::zlink_framework_options_t &options, const role_config_t &config, bool calls_channel)
{
    auto mesh = options.add_route_mesh (*config.mesh_name);
    mesh.set_automatic_routing_id_prefix ("perf-spot");
    mesh.listen (config.transport_endpoints.at ("mesh"));
    if (calls_channel)
        mesh.channel (*config.channel_name).client ();
    mesh.objects ()
      .server ()
      .template add_spot_factory<TSpot, role_t, fw::route_client_t> (perf_spot_type)
      .set_execution_mode (fw::user_spot_execution_mode_t::spot_wide)
      .set_stable_type_limit (static_cast<std::int32_t> (config.spot_ids.size ()))
      .disable_relocation ();
}

// The public create results of this cell (§16.1 objectsReady): the manager result and the mesh placement count.
struct spot_objects_t
{
    bool ready = false;
    json evidence = json::array ();
};

// Setup: every SpotId of the cell is created through the public manager. Not part of any measured latency.
// A role that probes reports objectsReady only after its probes, so warmup never overlaps a probe.
inline spot_objects_t create_spots (role_t &role, const std::atomic<bool> &stopping)
{
    const auto &config = role.config;
    wait_for_public (role, stopping, [&] { return role.mesh.load () != nullptr; }, "the RouteMesh runtime");
    auto &manager = role.service<fw::spot_manager_t> ();
    json created = json::array ();
    for (const auto &spot_id : config.spot_ids) {
        const auto result = manager.get_or_create (spot_id, perf_spot_type)
                              .timeout (std::chrono::milliseconds (config.workload.setup_timeout_ms))
                              .async ()
                              .result ()
                              .value ();
        if (result.state == fw::spot_create_state_t::rejected)
            throw std::runtime_error ("Spot " + spot_id + " was rejected.");
        created.push_back ({{"spotId", spot_id}, {"state", static_cast<int> (result.state)}, {"meshName", std::string (result.spot.mesh_name ())}});
    }
    wait_for_public (role, stopping, [&] {
        const auto placement = role.mesh.load ()->snapshot (*config.mesh_name).placement;
        return placement.is_available && placement.active_spot_count >= config.spot_ids.size ();
    }, "the User Spots to be active");
    const auto placement = role.mesh.load ()->snapshot (*config.mesh_name).placement;
    return {true,
            json::array ({{{"kind", "spotCreate"}, {"source", "spot_manager_t.get_or_create.async"}, {"observedValue", created}},
                          {{"kind", "spotPlacement"}, {"source", "route_mesh_runtime_t.snapshot.placement"},
                           {"observedValue", {{"isAvailable", placement.is_available}, {"activeSpotCount", placement.active_spot_count},
                                              {"expectedSpots", config.spot_ids.size ()}}}}})};
}

// The Object Client side of setup: the manager resolves every SpotId once the public RouteMesh status is ready. Status and the
// resolve are polled; the probe call itself is never retried.
inline json find_spots (role_t &role, const std::atomic<bool> &stopping)
{
    const auto &config = role.config;
    wait_for_public (role, stopping, [&] { return role.mesh.load () != nullptr && role.mesh.load ()->snapshot (*config.mesh_name).is_ready; }, "the RouteMesh");
    auto &manager = role.service<fw::spot_manager_t> ();
    json found = json::array ();
    for (const auto &spot_id : config.spot_ids) {
        std::optional<fw::spot_ref_t> spot;
        wait_for_public (role, stopping, [&] { spot = manager.find (spot_id).result ().value (); return spot.has_value (); }, "a User Spot");
        found.push_back ({{"spotId", spot_id}, {"meshName", std::string (spot->mesh_name ())}, {"nodeRid", std::string (spot->node_rid ().value ())}});
    }
    return found;
}

inline void publish_spots (role_t &role, const spot_objects_t &objects)
{
    role.objects->set (objects.ready, "The Object Server has not activated every User Spot of this cell.", objects.evidence);
}

// The User Spot that answers a typed PerfEchoRequest with the typed echo at once: the target of §10.3 and the local
// echo Spot of §10.7. The typed request handler is the only application code on the Spot.
class perf_echo_spot_t final : public perf_spot_base_t<perf_echo_spot_t>
{
  public:
    perf_echo_spot_t (fw::spot_context_t context, role_t &role, fw::route_client_t &) :
        perf_spot_base_t (std::move (context)), _role (role)
    {
    }
    void configure () override { _context.handlers ().add_handler<&perf_echo_spot_t::echo> (echo_request_t::packet_name); }

    echo_reply_t echo (const echo_request_t &request)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            measurement.validate_request (request);
            const auto reply = payload_pattern_t::reply (request, received);
            measurement.record_reply (request);
            if (request.phase == "measured")
                _role.metrics.count ("spot.applicationHandlerEntries");
            if (measurement.phase () == "setup" && !_role.config.source)
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"},
                                                               {"source", "perf_echo_spot_t typed Spot request handler (echo_request_t -> echo_reply_t)"},
                                                               {"observedValue", request.correlation_id}}}));
            return reply;
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    role_t &_role;
};
} // namespace perf
