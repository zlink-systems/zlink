/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Actor echo objects shared by the CS (§10.1, §10.2) and Actor (§10.9, §10.10) Object Servers, and the STREAM session
// that relays to them. The Actor holds no state: every measured call is the typed echo of the Entry Spot's Actor handler.

#include <perf/server/server_application.hpp>

namespace perf
{
class perf_actor_t final : public fw::actor_t
{
  public:
    explicit perf_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};

struct perf_actor_factory_t final : fw::actor_factory_t<perf_actor_t>
{
    fw::task_t<std::shared_ptr<perf_actor_t>> create (fw::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<perf_actor_t> (std::move (context));
    }
};

inline constexpr const char *perf_actor_type = "perf-actor";

// The request cells answer with the typed reply; the send-send cell answers with a public Channel send (§10.10).
class perf_entry_spot_t final : public fw::entry_spot_t<perf_actor_t>
{
  public:
    perf_entry_spot_t (fw::entry_spot_context_t context, role_t &role, fw::route_client_t &route) :
        _context (std::move (context)), _role (role), _route (route)
    {
    }
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        if (_role.config.mode == "send-send")
            _context.handlers ().add_actor_send<&perf_entry_spot_t::echo_send> (echo_request_t::packet_name);
        else
            _context.handlers ().add_actor_request<&perf_entry_spot_t::echo_request> (echo_request_t::packet_name);
    }
    fw::task_t<void> on_actor_joined (perf_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (perf_actor_t &) override { co_return; }

    // §10.1, §10.2, §10.9: the handler return value is the reply.
    echo_reply_t echo_request (perf_actor_t &, fw::message_context_t &, const echo_request_t &request)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            measurement.validate_request (request);
            const auto reply = payload_pattern_t::reply (request, received);
            measurement.record_reply (request);
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"},
                                                               {"source", "perf_entry_spot_t actor request handler (echo_request_t -> echo_reply_t)"},
                                                               {"observedValue", request.correlation_id}}}));
            return reply;
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

    // §10.10: the Actor answers through the public Channel client; the caller is the return channel Server.
    fw::task_t<void> echo_send (perf_actor_t &, fw::message_context_t &, const echo_request_t &request)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            measurement.validate_request (request, _role.config.channel_name);
            const auto reply = payload_pattern_t::reply (request, received);
            measurement.record_application_call (request, "send");
            co_await _route.send_to_channel (*request.return_channel, reply).async ();
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"},
                                                               {"source", "route_client_t.send_to_channel(returnChannel).async"},
                                                               {"observedValue", request.correlation_id}}}));
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    fw::entry_spot_context_t _context;
    role_t &_role;
    fw::route_client_t &_route;
};

// The Actors are created as Entry Spot members and never move (perf never measures relocation).
inline void add_perf_actors (fw::mesh_node_builder_t &mesh)
{
    mesh.objects ()
      .server ()
      .add_entry_spot<perf_entry_spot_t, role_t, fw::route_client_t> ()
      .add_actor_factory<perf_actor_t, perf_actor_factory_t> (perf_actor_type)
      .disable_relocation ();
}

// The Actor role's objectsReady (§16.1): the Actors this process hosts, read from the public RouteMesh placement
// status by a background poll during setup, never from inside a handler turn.
class actor_placement_watcher_t final : public fw::hosted_service_t
{
  public:
    explicit actor_placement_watcher_t (role_t &role) : _role (role) {}
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        auto *mesh = &services.get_required<fw::route_mesh_runtime_t> ();
        _thread = std::thread ([this, mesh] {
            while (!_stop.load () && _role.measurement.phase () == "setup") {
                {
                    const auto placement = mesh->snapshot (*_role.config.mesh_name).placement;
                    _role.objects->set (placement.is_available && placement.active_actor_count > 0, "No Actor is active on this Object Server.",
                                        json::array ({{{"kind", "actorPlacement"}, {"source", "route_mesh_runtime_t.snapshot.placement"},
                                                       {"observedValue", {{"isAvailable", placement.is_available},
                                                                          {"activeActorCount", placement.active_actor_count},
                                                                          {"expectedActors", _role.config.actor_ids.size ()}}}}}));
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (100));
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
    std::atomic<bool> _stop{false};
    std::thread _thread;
};

// The Session role's create and bind (§10.1, §10.2 preparation): the connector's setup probe names its clientId, which
// selects the Actor ID of that connector; the Actor is created through the public manager and bound to the session
// before the probe itself is relayed. Setup latencies are kept apart from the measured operations.
class session_actor_setup_t
{
  public:
    explicit session_actor_setup_t (role_t &role) : _role (role) {}

    fw::task_t<fw::session_actor_t> prepare (fw::stream_t &stream, const zlink::message_t &probe)
    {
        auto &measurement = _role.measurement;
        if (measurement.phase () != "setup")
            throw std::logic_error ("Actors are created and bound during setup only.");
        try {
            const auto request = probe.parse_json<echo_request_t> ();
            if (request.client_id < 0 || static_cast<std::size_t> (request.client_id) >= _role.config.actor_ids.size ())
                throw validation_error_t ("IdentityMismatch", "clientId has no Actor ID in this cell.");
            const auto timeout = std::chrono::milliseconds (_role.config.workload.setup_timeout_ms);
            const auto create_started = now_ticks ();
            const auto result = co_await _role.service<fw::actor_manager_t> ()
                                  .get_or_create (fw::actor_id_t (_role.config.actor_ids[static_cast<std::size_t> (request.client_id)]), perf_actor_type)
                                  .in_mesh (*_role.config.mesh_name)
                                  .timeout (timeout)
                                  .async ();
            std::optional<fw::actor_ref_t> ref;
            bool was_created = false;
            if (const auto *created = std::get_if<fw::actor_create_created_t> (&result)) {
                ref = created->actor;
                was_created = true;
            }
            else if (const auto *existing = std::get_if<fw::actor_create_existing_t> (&result))
                ref = existing->actor;
            else
                throw std::runtime_error ("Actor creation was rejected.");
            const auto bind_started = now_ticks ();
            auto binding = co_await stream.actors ().bind_or_get (*ref).timeout (timeout).async ();
            record (was_created, bind_started - create_started, now_ticks () - bind_started);
            co_return binding;
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            {
                std::lock_guard lock (_gate);
                ++_failed;
            }
            publish ();
            throw;
        }
    }

  private:
    void record (bool was_created, std::int64_t create, std::int64_t bind)
    {
        {
            std::lock_guard lock (_gate);
            (was_created ? _created : _existing)++;
            ++_bound;
            _create_ns += create;
            _create_max_ns = std::max (_create_max_ns, create);
            _bind_ns += bind;
            _bind_max_ns = std::max (_bind_max_ns, bind);
        }
        publish ();
    }
    void publish ()
    {
        std::lock_guard lock (_gate);
        _role.objects->set (
          _bound > 0 && _failed == 0, _failed > 0 ? "Actor create or bind failed." : "No Actor is bound to a session yet.",
          json::array ({{{"kind", "actorCreateAndBind"}, {"source", "actor_manager_t.get_or_create + session_actor_manager_t.bind_or_get"},
                         {"observedValue", {{"created", _created}, {"existing", _existing}, {"bound", _bound}, {"failed", _failed},
                                            {"expectedActors", _role.config.actor_ids.size ()},
                                            {"createMeanMs", _bound == 0 ? 0.0 : static_cast<double> (_create_ns) / 1e6 / static_cast<double> (_bound)},
                                            {"createMaxMs", static_cast<double> (_create_max_ns) / 1e6},
                                            {"bindMeanMs", _bound == 0 ? 0.0 : static_cast<double> (_bind_ns) / 1e6 / static_cast<double> (_bound)},
                                            {"bindMaxMs", static_cast<double> (_bind_max_ns) / 1e6}}}}}));
        // The Session role has no typed reply of its own: its setup probe is the admitted relay of a bound Actor.
        if (_bound > 0)
            _role.measurement.set_setup_evidence (json::array ({{{"kind", "relayAdmission"}, {"source", "session_actor_t.relay_request.async"},
                                                                 {"observedValue", {{"bound", _bound}}}}}));
    }

    role_t &_role;
    std::mutex _gate;
    std::uint64_t _created = 0, _existing = 0, _bound = 0, _failed = 0;
    std::int64_t _create_ns = 0, _create_max_ns = 0, _bind_ns = 0, _bind_max_ns = 0;
};

// §10.1/§10.2: the session relays every packet to the Actor bound to it; the Actor handler return value is the reply
// the session writes to the original STREAM request (C++ `relay_request`, the session writes the reply itself).
class perf_actor_relay_session_t final : public fw::packet_stream_session_t
{
  public:
    perf_actor_relay_session_t (role_t &role, session_actor_setup_t &setup) : _role (role), _setup (setup) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &error) override
    {
        _role.measurement.record_diagnostic (std::make_exception_ptr (std::runtime_error (std::string ("STREAM ") + std::string (error.message ()))));
        co_return;
    }

    fw::task_t<void> on_packet (fw::stream_t &stream, const fw::session_message_context_t &dispatch, const zlink::message_t &payload) override
    {
        auto &measurement = _role.measurement;
        try {
            zlink::message_t reply;
            if (dispatch.actor || _binding) {
                if (dispatch.actor)
                    _binding = *dispatch.actor;
                // Called before any suspension: this overload takes the dispatch state of the packet being handled.
                reply = co_await _binding->relay_request (payload).async ();
            }
            else {
                _binding = co_await _setup.prepare (stream, payload);
                reply = co_await _binding->relay_request (dispatch.packet_name, payload).async ();
            }
            co_await stream.reply_packet (reply).async ();
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    role_t &_role;
    session_actor_setup_t &_setup;
    std::optional<fw::session_actor_t> _binding;
};
} // namespace perf
