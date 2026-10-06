/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.9 actor-no-bind-request-echo. Question: what a global ActorId request costs (address lookup and remote request) when it
// is reached without any Session binding. Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client, this file) x1,
// Actor (Object Server, actor_server/) x1. One operation: `actor_client_t.request(actorId,dto).async<PerfEchoReply>()` starts
// and ends when the typed echo has been validated. request, ordinary; 4096 bytes; one unbound Actor per logical stream,
// created during setup. Store: run Docker Redis. Null: physical connections, Spot, worker, fanout and actor.sourceAdmission
// (no send).

#include "actor_caller_setup.hpp"

namespace perf
{
class actor_no_bind_request_echo_scenario_t
{
  public:
    explicit actor_no_bind_request_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto created = create_actors (_role, stopping);
        auto &actors = _role.service<fw::actor_client_t> ();
        // §5: one probe echo per prepared target (bounded by connect-concurrency).
        for_each_concurrently (*config.workload.logical_streams, *config.workload.connect_concurrency, [&] (int stream) {
            const auto request = measurement.request (stream, _sequences.next (stream), true);
            const auto reply = actors.request (fw::actor_id_t (config.actor_ids[static_cast<std::size_t> (stream)]), request)
                                 .timeout (std::chrono::milliseconds (config.workload.setup_timeout_ms))
                                 .async<echo_reply_t> ()
                                 .result ()
                                 .value ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
        });
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "actor_client_t.request.async<PerfEchoReply>"},
                                                       {"observedValue", {{"probes", *config.workload.logical_streams}, {"streams", *config.workload.logical_streams}}}}}));
        // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
        json evidence = json::array ({created});
        for (const auto &item : measurement.setup_evidence ())
            evidence.push_back (item);
        _role.objects->set (true, "", evidence);
    }

    void run (const loops_t &loops)
    {
        spawn_request_streams (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &actors = _role.service<fw::actor_client_t> ();
        const auto &config = _role.config;
        const fw::actor_id_t actor_id (config.actor_ids[static_cast<std::size_t> (stream)]);
        auto request = measurement.request (stream, _sequences.next (stream));
        std::int64_t started = 0;
        if (!measurement.begin_operation (started))
            co_return;
        request.sent_ticks = dec (started);
        std::exception_ptr error;
        try {
            const auto reply = co_await actors.request (actor_id, request)
                                 .timeout (measurement.call_timeout ())
                                 .async<echo_reply_t> ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
        }
        catch (...) {
            error = std::current_exception ();
        }
        measurement.complete_operation (started, error);
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
