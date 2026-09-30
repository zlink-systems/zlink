/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.10 actor-no-bind-send-send-echo. Question: what do the source admission of a global-ActorId send and the application
// echo round trip cost, measured apart? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client plus the Server of a
// run/cell-only return ChannelName, this file) x1, Actor (Object Server, actor_server/) x1.
// One operation: the correlation is registered and `actor_client_t.send(actorId,dto).async()` starts; it ends when the return
// Channel handler validates the echo (§13). Separately, actor.sourceAdmission.* is the same call start to the send terminal.
// send-send, ordinary; 4096 bytes; one unbound Actor per logical stream. Store: run Docker Redis. Null: physical
// connections, Spot, worker, fanout; the remote mailbox acceptance time is not publicly observable.

#include "actor_caller_setup.hpp"

namespace perf
{
class actor_no_bind_send_send_echo_scenario_t
{
  public:
    explicit actor_no_bind_send_send_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        role.metrics.latency ("sourceAdmissionMs", "actor.sourceAdmission.latency");
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto created = create_actors (_role, stopping);
        auto &actors = _role.service<fw::actor_client_t> ();
        auto &correlations = *_role.correlations;
        // §5: one probe echo per prepared target (bounded by connect-concurrency).
        for_each_concurrently (*config.workload.logical_streams, *config.workload.connect_concurrency, [&] (int stream) {
            auto request = measurement.request (stream, _sequences.next (stream), true);
            request.return_channel = config.channel_name;
            const auto entry = correlations.register_request (request, now_ticks ());
            actors.send (fw::actor_id_t (config.actor_ids[static_cast<std::size_t> (stream)]), request).async ().result ().value ();
            const auto [error, completed] = correlations.complete (entry).result ().value ();
            (void) completed;
            if (error)
                std::rethrow_exception (error);
        });
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "actor_client_t.send -> return Channel send handler"},
                                                       {"observedValue", {{"probes", *config.workload.logical_streams}, {"streams", *config.workload.logical_streams}}}}}));
        // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
        json evidence = json::array ({created});
        for (const auto &item : measurement.setup_evidence ())
            evidence.push_back (item);
        _role.objects->set (true, "", evidence);
    }

    void run (const loops_t &loops)
    {
        spawn_stream_loops (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &actors = _role.service<fw::actor_client_t> ();
        auto &correlations = *_role.correlations;
        const auto &config = _role.config;
        const fw::actor_id_t actor_id (config.actor_ids[static_cast<std::size_t> (stream)]);
        while (measurement.can_issue ()) {
            auto request = measurement.request (stream, _sequences.next (stream));
            request.return_channel = config.channel_name;
            std::int64_t started = 0;
            if (!measurement.begin_operation (started, "send"))
                break;
            request.sent_ticks = dec (started);
            const auto entry = correlations.register_request (request, started); // §13: registered right before the first public send
            try {
                co_await actors.send (actor_id, request).async ();
                _role.metrics.record ("sourceAdmissionMs", started, now_ticks ());
                correlations.first_send_ended (entry, nullptr);
            }
            catch (...) {
                correlations.first_send_ended (entry, std::current_exception ());
            }
            const auto [result, completed] = co_await correlations.complete (entry); // the return Channel handler decides
            measurement.complete_operation (started, result, completed);
        }
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
