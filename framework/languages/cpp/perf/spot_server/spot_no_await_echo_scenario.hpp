/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.7 spot-no-await-echo (also the §11.3 local Spot reference). Question: what the public Spot client costs from the caller
// to a User Spot in the same process that echoes at once; not a pure mailbox cost. Roles: HTTP Client x1, Spot process
// (Object Server + local application driver, this file) x1; no Channel, Actor or worker. One operation: the local
// `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()` until the typed echo is validated (codec and local
// dispatch included, the HTTP trigger is not). no-await: the caller is an ordinary request and the handler replies with the
// typed echo at once. Payload 1024 bytes. Setup: only this Object Server can place the Spots. Store: run Docker Redis
// (Spot addresses). Null: remote call, worker, Actor, fanout; mailbox depth and real turns have no public observation; the
// driver histogram is not kept because this interval is already the primary latency.

#include <perf/server/spot_role.hpp>

namespace perf
{
class spot_no_await_echo_scenario_t
{
  public:
    explicit spot_no_await_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        role.metrics.counters ({"spot.applicationHandlerEntries"}).spot_internals_unsupported ();
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto objects = create_spots (_role, stopping);
        auto &route = _role.service<fw::route_client_t> ();
        json probes = json::array ();
        for (std::size_t target = 0; target < config.spot_ids.size (); ++target) {
            const auto request = measurement.request (static_cast<int> (target), _sequences.next (static_cast<int> (target)), true);
            const auto reply = route.request_to_spot (config.spot_ids[target], request)
                                 .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                                 .async<echo_reply_t> ()
                                 .result ()
                                 .value ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
            probes.push_back ({{"correlationId", request.correlation_id}, {"receivedTicks", reply.received_ticks}, {"clockDomainId", reply.clock_domain_id}});
        }
        publish_spots (_role, objects); // objectsReady only after every probe, so warmup never overlaps one
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "route_client_t.request_to_spot.async<PerfEchoReply>"}, {"observedValue", probes}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_stream_loops (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        const auto &config = _role.config;
        const auto &spot_id = config.spot_ids[static_cast<std::size_t> (stream) % config.spot_ids.size ()];
        while (measurement.can_issue ()) {
            auto request = measurement.request (stream, _sequences.next (stream));
            std::int64_t started = 0;
            if (!measurement.begin_operation (started))
                break;
            request.sent_ticks = dec (started);
            std::exception_ptr error;
            try {
                const auto reply = co_await route.request_to_spot (spot_id, request)
                                     .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                                     .async<echo_reply_t> ();
                payload_pattern_t::validate_identity (request, reply);
                measurement.pattern ().validate (reply.payload);
            }
            catch (...) {
                error = std::current_exception ();
            }
            measurement.complete_operation (started, error);
        }
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
