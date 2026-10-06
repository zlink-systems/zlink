/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Perf spec §6.3, §16: what every role process shares. The application-owned admin endpoints (`/perf/ready`,
// `/perf/stats`, `/perf/reset`, `/app/perf/start`), the readiness evidence built only from public status, the public
// status projection, and the framework options every role starts from. No measured call lives here.

#include <perf/send_send_correlation.hpp>

#include <algorithm>
#include <zlink/core/api.h>
#include <zlink/locations/redis.hpp>

namespace fw = zlink::framework;

namespace perf
{

// The role's own statement that its cell objects (Spot, Actor, subscriptions) are not yet prepared (§16.1 objectsReady).
// The role replaces the statement as its public create/bind results arrive; the evidence lists those results.
class objects_readiness_t
{
  public:
    objects_readiness_t (bool ready, std::string reason) :
        _state (
          std::make_shared<const state_t> (state_t{ready, std::move (reason), json::array ()}))
    {
    }
    bool ready () const { return std::atomic_load (&_state)->ready; }
    std::string reason () const { return std::atomic_load (&_state)->reason; }
    json evidence () const { return std::atomic_load (&_state)->evidence; }
    void set (bool ready, std::string reason, json evidence)
    {
        std::atomic_store (&_state, std::make_shared<const state_t> (
                                      state_t{ready, std::move (reason), std::move (evidence)}));
    }

  private:
    struct state_t
    {
        bool ready;
        std::string reason;
        json evidence;
    };
    std::shared_ptr<const state_t> _state;
};

// Everything the HTTP handlers and the hosted services of one role process reach: one singleton, so a handler's
// constructor has one dependency and the role's `main` shows the wiring.
class role_t
{
  public:
    role_t (role_config_t role_config, bool has_objects_readiness) :
        config (std::move (role_config)), measurement (config, config.source), metrics (measurement)
    {
        if (has_objects_readiness)
            objects.emplace (false, "Objects have not been prepared yet.");
        measurement.set_sample_public_state ([this] { return sample_public_state (); });
    }
    role_t (const role_t &) = delete;
    role_t &operator= (const role_t &) = delete;

    role_config_t config;
    measurement_t measurement;
    scenario_metrics_t metrics;
    // The harness correlation table of a send/send role (§13); null on the other roles.
    std::unique_ptr<send_send_correlation_t> correlations;
    std::optional<objects_readiness_t> objects;
    // Set by `runtime_binding_service_t`, the first hosted service, so scenario services always see them set; the admin HTTP
    // status, which can answer before the host starts, checks for null. The public runtime interfaces of this process.
    std::atomic<fw::framework_runtime_t *> runtime{nullptr};
    std::atomic<fw::route_mesh_runtime_t *> mesh{nullptr};
    std::atomic<fw::client_server_runtime_t *> client_server{nullptr};
    // The host service provider (a copy; the singletons behind it are the host own): scenario code resolves the public
    // messaging and manager interfaces of this process from it.
    std::optional<fw::service_provider_t> services;
    template <typename T> T &service () { return services->get_required<T> (); }
    // What `/app/perf/start` launches (§4.2): null on roles that only receive.
    workload_fn_t workload;

    // ---- public status projection (§14 publicStatus; the language's own field names) ----
    static json host_status (const fw::framework_runtime_status_t &s)
    {
        const auto &q = s.capacity.application_job_queue;
        const auto &h = s.capacity.core_hwm;
        return {{"state", static_cast<int> (s.state)},
                {"isReady", s.is_ready},
                {"acceptingWork", s.accepting_work},
                {"sequence", dec (s.sequence)},
                {"capacity",
                 {{"measurementEpoch", dec (s.capacity.measurement_epoch)},
                  {"coreHwm",
                   {{"effectiveBudgetBytes", dec (h.effective_budget_bytes)},
                    {"totalAppliedHwmBytes", dec (h.total_applied_hwm_bytes)},
                    {"coreQueueAccountedBytes", dec (h.core_queue_accounted_bytes)},
                    {"currentAccountedBytes", dec (h.current_accounted_bytes)},
                    {"peakAccountedBytes", dec (h.peak_accounted_bytes)},
                    {"blockedRatioPpm", dec (h.blocked_ratio_ppm)},
                    {"activeDirectionalQueueCount", dec (h.active_directional_queue_count)}}},
                  {"applicationJobQueue",
                   {{"effectiveProcessorCount", q.effective_processor_count},
                    {"effectiveMaxQueuedApplicationJobs", q.effective_max_queued_application_jobs},
                    {"queuedApplicationJobs", q.queued_application_jobs},
                    {"permitsInUse", q.permits_in_use},
                    {"peakPermitsInUse", q.peak_permits_in_use},
                    {"capacityWaiters", q.capacity_waiters},
                    {"capacityWaitCount", dec (q.capacity_wait_count)},
                    {"capacityWaitDurationNs",
                     dec (static_cast<std::int64_t> (q.capacity_wait_duration.count ()))},
                    {"pressureState", static_cast<int> (q.pressure_state)}}}}}};
    }
    static json mesh_status (const fw::mesh_node_snapshot_t &m)
    {
        json channels = json::array ();
        for (const auto &c : m.channels)
            channels.push_back ({{"channelName", c.channel_name},
                                 {"isReady", c.is_ready},
                                 {"readyTargetCount", c.ready_target_count}});
        return {{"meshName", m.mesh_name},
                {"state", static_cast<int> (m.state)},
                {"isReady", m.is_ready},
                {"readyPeerCount", m.ready_peer_count},
                {"channels", channels},
                {"peerCount", m.peers.size ()},
                {"placement",
                 {{"isAvailable", m.placement.is_available},
                  {"activeActorCount", m.placement.active_actor_count},
                  {"activeSpotCount", m.placement.active_spot_count}}}};
    }
    static json client_server_status (const fw::client_server_channel_snapshot_t &c)
    {
        static const char *const roles[] = {"Client", "Server", "ClientAndServer"};
        return {{"channelName", c.channel_name},
                {"localRole", roles[static_cast<int> (c.local_role)]},
                {"state", static_cast<int> (c.state)},
                {"isReady", c.is_ready},
                {"readyTargetCount", c.ready_target_count},
                {"targetCount", c.targets.size ()}};
    }

    // Select topology from the role config; runtime status is present only after its public interface is bound.
    std::optional<std::string> observed_topology () const { return config.topology; }
    json public_status () const
    {
        json status = {
          {"host", runtime.load () ? host_status (runtime.load ()->status ()) : json (nullptr)}};
        const auto topology = observed_topology ();
        if (topology == "routemesh" && mesh.load () && config.mesh_name)
            status["routeMesh"] = mesh_status (mesh.load ()->snapshot (*config.mesh_name));
        else if (topology == "clientserver" && client_server.load () && config.channel_name)
            status["clientServer"] =
              client_server_status (client_server.load ()->snapshot (*config.channel_name));
        return status;
    }
    json bound_transport_endpoints () const
    {
        json endpoints = json::object ();
        const auto *host = runtime.load ();
        if (!host)
            return endpoints;
        for (const auto &entry : config.transport_endpoints) {
            const auto &key = entry.first;
            fw::listener_kind_t kind;
            std::string name;
            if (key == "stream") {
                kind = fw::listener_kind_t::stream;
                name = "perf-session";
            } else if (key == "mesh") {
                kind = fw::listener_kind_t::route_mesh;
                name = config.mesh_name.value_or ("");
            } else if (key == "clientserver") {
                kind = fw::listener_kind_t::client_server;
                name = config.channel_name.value_or ("");
            } else if (key == "fanout") {
                kind = fw::listener_kind_t::fanout;
                name = config.channel_name.value_or ("");
            } else {
                throw std::runtime_error ("Unknown perf listener key '" + key + "'.");
            }
            endpoints[key] = host->listener_status (kind, std::move (name)).endpoint;
        }
        return endpoints;
    }
    json sample_public_state () const
    {
        const auto *host = runtime.load ();
        if (!host)
            return nullptr;
        const auto status = host->status ();
        return {{"observedTicks", dec (now_ticks ())},
                {"state", static_cast<int> (status.state)},
                {"isReady", status.is_ready},
                {"acceptingWork", status.accepting_work},
                {"pressureState",
                 static_cast<int> (status.capacity.application_job_queue.pressure_state)}};
    }

    // §16.1 PerfReady, from public status and the role's own typed-probe / object evidence only.
    json ready () const
    {
        const bool host_ready = runtime.load () && runtime.load ()->status ().is_ready;
        bool infrastructure = host_ready;
        const auto topology = observed_topology ();
        if (host_ready && topology == "routemesh" && mesh.load () && config.mesh_name) {
            const auto snapshot = mesh.load ()->snapshot (*config.mesh_name);
            // Channel messaging §3: RouteMesh excludes the sending node itself from candidates. Only the source needs a
            // selectable remote target; the receiver proves dispatch by echo. A send/send source that is itself the
            // only Server of its return ChannelName has no remote target by design (awaitRemoteTargets=false).
            bool target = !config.source || !config.await_remote_targets;
            for (const auto &c : snapshot.channels)
                target = target
                         || (c.channel_name == config.channel_name && c.is_ready
                             && c.ready_target_count > 0);
            infrastructure = snapshot.is_ready && target;
        } else if (host_ready && topology == "clientserver" && client_server.load ()
                   && config.channel_name) {
            const auto snapshot = client_server.load ()->snapshot (*config.channel_name);
            infrastructure = snapshot.is_ready;
        }
        if (config.object_role == "ObjectClient" && config.mesh_name) {
            const auto *mesh_runtime = mesh.load ();
            infrastructure = infrastructure && mesh_runtime
                             && mesh_runtime->snapshot (*config.mesh_name).ready_peer_count > 0;
        }
        const auto setup_evidence = measurement.setup_evidence ();
        const bool probe = setup_evidence.is_array () && !setup_evidence.empty ();
        const bool objects_ready = objects ? objects->ready () : true;
        const json transport_endpoints =
          infrastructure ? bound_transport_endpoints () : json::object ();
        json evidence = json::array ({{{"kind", "publicStatus"},
                                       {"source", "public Framework runtime status"},
                                       {"observedValue", public_status ()}}});
        if (!transport_endpoints.empty ())
            evidence.push_back ({{"kind", "boundTransportEndpoints"},
                                 {"source", "public Framework listener status"},
                                 {"observedValue", transport_endpoints}});
        if (objects)
            for (const auto &item : objects->evidence ())
                evidence.push_back (item);
        for (const auto &item : setup_evidence)
            evidence.push_back (item);
        for (const auto &item : measurement.error_evidence ())
            evidence.push_back (item);
        json reasons = json::array ();
        if (!infrastructure)
            reasons.push_back ("Public host/channel/listener infrastructure is not ready.");
        if (!objects_ready)
            reasons.push_back (objects->reason ());
        if (!probe)
            reasons.push_back (
              "No successful typed probe echo or fanout warmup marker has been observed.");
        if (measurement.has_errors ())
            reasons.push_back ("Application preparation or phase failed.");
        return {{"runId", config.run_id},
                {"cellId", config.cell_id},
                {"role", config.role},
                {"roleInstance", config.role_instance},
                {"infrastructureReady", infrastructure},
                {"objectsReady", objects_ready},
                {"consumersReady", probe},
                {"ready", infrastructure && objects_ready && probe && !measurement.has_errors ()},
                {"observedAtUnixMs", unix_ms ()},
                {"evidence", evidence},
                {"reasons", reasons}};
    }
};

inline fw::http_response_t json_response (const json &body, int status = 200)
{
    fw::http_response_t response;
    response.status = status;
    response.body = body.dump ();
    response.content_type = "application/json";
    return response;
}

// The Core version this process actually loaded (public binding API), reported in provenance (§19).
inline std::string loaded_core_version ()
{
    int major = 0, minor = 0, patch = 0;
    zlink_version (&major, &minor, &patch);
    return std::to_string (major) + "." + std::to_string (minor) + "." + std::to_string (patch);
}

class ready_handler_t
{
  public:
    explicit ready_handler_t (role_t &role) : _role (role) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        try {
            return json_response (_role.ready ());
        }
        catch (const std::exception &error) {
            return json_response ({{"reason", error.what ()}}, 500);
        }
    }

  private:
    role_t &_role;
};

class stats_handler_t
{
  public:
    explicit stats_handler_t (role_t &role) : _role (role) {}
    fw::http_response_t handle (const fw::http_request_t &request)
    {
        try {
            // The runner's final phase read seals sequence originals after the measured window has ended.
            _role.measurement.set_final_snapshot (request.query_values.contains ("final"));
            auto snapshot = _role.measurement.snapshot (_role.public_status ());
            snapshot["provenance"]["coreVersion"] = loaded_core_version ();
            return json_response (snapshot);
        }
        catch (const std::exception &error) {
            return json_response ({{"reason", error.what ()}}, 500);
        }
    }

  private:
    role_t &_role;
};

class reset_handler_t
{
  public:
    explicit reset_handler_t (role_t &role) : _role (role) {}
    fw::http_response_t handle (const fw::http_request_t &request)
    {
        reset_request_t body;
        try {
            body = json::parse (request.body).get<reset_request_t> ();
        }
        catch (const std::exception &error) {
            return json_response ({{"reason", error.what ()}}, 400);
        }
        try {
            const auto [reply, status] =
              _role.measurement.reset (body, [this] () -> std::optional<std::uint64_t> {
                  auto *host = _role.runtime.load ();
                  if (!host)
                      return std::nullopt;
                  host->reset_capacity_metrics ();
                  return host->status ().capacity.measurement_epoch;
              });
            return json_response (reply, status);
        }
        catch (const validation_error_t &error) {
            return json_response ({{"reason", error.what ()}}, 400);
        }
    }

  private:
    role_t &_role;
};

class start_handler_t
{
  public:
    explicit start_handler_t (role_t &role) : _role (role) {}
    fw::http_response_t handle (const fw::http_request_t &request)
    {
        trigger_request_t trigger;
        try {
            trigger = json::parse (request.body).get<trigger_request_t> ();
            (void) parse_u64 (trigger.reset_seq);
        }
        catch (const std::exception &error) {
            return json_response ({{"reason", error.what ()}}, 400);
        }
        try {
            const auto ready = _role.ready ();
            // §16.1: warmup starts after infrastructure and objects; only the measured barrier needs consumersReady (PS marker).
            const bool ok = trigger.phase == "warmup" ? ready["infrastructureReady"].get<bool> ()
                                                          && ready["objectsReady"].get<bool> ()
                                                      : ready["ready"].get<bool> ();
            if (!ok)
                return json_response (
                  {{"reason", "Readiness evidence is incomplete."}, {"ready", ready}}, 409);
            const auto reply = _role.measurement.start (trigger, _role.workload);
            return json_response (json (reply), reply.accepted ? 200 : 409);
        }
        catch (const std::exception &error) {
            return json_response ({{"reason", error.what ()}}, 500);
        }
    }

  private:
    role_t &_role;
};

// Binds the public runtime interfaces to the role when the host starts; the hosted service owns no behaviour beyond that.
class runtime_binding_service_t final : public fw::hosted_service_t
{
  public:
    explicit runtime_binding_service_t (role_t &role) : _role (role) {}
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _role.services = services;
        if (const auto mesh = services.get<fw::route_mesh_runtime_t> ())
            _role.mesh = &mesh->get ();
        if (const auto client_server = services.get<fw::client_server_runtime_t> ())
            _role.client_server = &client_server->get ();
        _role.runtime = &services.get_required<fw::framework_runtime_t> ();
        co_return;
    }
    void stop () noexcept override {}

  private:
    role_t &_role;
};

// A source role prepares its objects and probes after the host started (§16.1). The prepare function runs on its own
// thread, so the hosted-service start returns at once; it reads public status and makes public blocking calls the way an
// application thread outside the runtime does (task.result()), never from inside a handler turn.
class prepare_service_t final : public fw::hosted_service_t
{
  public:
    prepare_service_t (role_t &role,
                       std::function<void (const std::atomic<bool> &stopping)> prepare) :
        _role (role), _prepare (std::move (prepare))
    {
    }
    fw::task_t<void> start (fw::service_provider_t &) override
    {
        _thread = std::thread ([this] {
            try {
                _prepare (_stop);
            }
            catch (...) {
                _role.measurement.record_diagnostic (std::current_exception ());
            }
        });
        co_return;
    }
    void request_stop () noexcept override { _stop = true; }
    void stop () noexcept override
    {
        _stop = true;
        if (_thread.joinable ())
            _thread.join ();
    }

  private:
    role_t &_role;
    std::function<void (const std::atomic<bool> &)> _prepare;
    std::atomic<bool> _stop{false};
    std::thread _thread;
};

// Polls a public-status predicate until it holds, the setup deadline passes or the process stops (setupTimeoutMs).
template <typename TPredicate>
void wait_for_public (const role_t &role,
                      const std::atomic<bool> &stopping,
                      TPredicate &&predicate,
                      const char *what)
{
    const auto deadline =
      now_ticks () + static_cast<std::int64_t> (role.config.workload.setup_timeout_ms) * 1'000'000;
    while (!predicate ()) {
        if (stopping.load ())
            throw std::runtime_error (std::string ("Stopped while waiting for ") + what);
        if (now_ticks () >= deadline)
            throw fw::framework_exception_t (fw::framework_error_kind_t::deadline_exceeded,
                                             std::string ("Setup deadline while waiting for ")
                                               + what);
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

// The framework options every role starts from (§5.2, §20): loopback hosts, the run's Redis when the cell needs a
// Store, the request timeout, and the admin/trigger HTTP listeners. Scenario code adds only its own topology.
inline void configure_base (fw::zlink_framework_options_t &options, role_t &role)
{
    const auto &config = role.config;
    options.set_default_request_timeout (
      std::chrono::milliseconds (config.workload.setup_timeout_ms));
    options.configure_network ().set_bind_host ("127.0.0.1");
    options.configure_network ().set_advertise_host (std::optional<std::string> ("127.0.0.1"));
    // Perf spec §20: the run-owned Docker Redis, one namespace per cell; only Store scenarios carry it.
    if (config.store)
        options.add_location_store<fw::redis::redis_location_store_t> ()
          .set_connection_string (config.store->endpoint)
          .set_key_prefix (config.store->ns + ":");
    // §16: the admin listener and the application trigger listener are separate URLs; both are served by this host's
    // HTTP hosting, which routes by path (the hosting exposes no per-listener routes).
    options.http ()
      .listen (config.metrics_url)
      .listen (config.application_trigger_url.substr (
        0, config.application_trigger_url.find ('/', std::string ("http://").size ())))
      .map_get<ready_handler_t> ("/perf/ready")
      .map_get<stats_handler_t> ("/perf/stats")
      .map_post<reset_handler_t> ("/perf/reset")
      .map_post<start_handler_t> ("/app/perf/start");
    if (config.diagnostics)
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
}

// Runs the role: registers the singleton, applies the shared options then the scenario's, and blocks until stopped.
template <typename TConfigure>
int run_role (std::unique_ptr<role_t> role_owner, TConfigure &&configure)
{
    auto app = fw::app_t::create ();
    auto &role = *role_owner;
    if (role.config.diagnostics)
        app.logging ().use_file (role.config.diagnostics->flow_file);
    auto &options = app.add_zlink_framework ();
    options.services ().add_singleton<role_t> (std::move (role_owner));
    configure_base (options, role);
    // The host starts hosted services in registration order, so the runtime binding comes first: every scenario service
    // reads the bound runtime interfaces.
    app.add_hosted_service (std::make_unique<runtime_binding_service_t> (role));
    configure (options, app);
    char program[] = "perf";
    char *argv[] = {program, nullptr};
    return app.run (1, argv);
}
} // namespace perf
