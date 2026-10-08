/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.3 s2s-channel-to-spot-request-echo. Question: what a Channel process pays to start a remote request to a global
// SpotId (address lookup, transfer, reply). Roles: HTTP Client x1, Channel (Object Client, this file) x1, Spot (Object
// Server, spot_server/) x1. One operation: `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()` until the typed
// echo is validated. request, ordinary; payload 4096 bytes; `streamId mod spotCount` picks the Spot.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot mailbox and turn internals are not
// observable through public status.

#include <perf/server/spot_role.hpp>

namespace perf
{
class s2s_channel_to_spot_request_echo_scenario_t
{
  public:
    explicit s2s_channel_to_spot_request_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        role.metrics.spot_internals_unsupported ();
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto found = find_spots (_role, stopping);
        auto &route = _role.service<fw::route_client_t> ();
        json probes = json::array ();
        for (std::size_t target = 0; target < config.spot_ids.size (); ++target) {
            const auto request = measurement.request (
              static_cast<int> (target), _sequences.next (static_cast<int> (target)), true);
            const auto reply = route.request_to_spot (config.spot_ids[target], request)
                                 .timeout (measurement.call_timeout ())
                                 .async<echo_reply_t> ()
                                 .result ()
                                 .value ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
            probes.push_back ({{"correlationId", request.correlation_id},
                               {"receivedTicks", reply.received_ticks},
                               {"clockDomainId", reply.clock_domain_id}});
        }
        _role.objects->set (
          true, "",
          json::array (
            {{{"kind", "spotFind"}, {"source", "spot_manager_t.find"}, {"observedValue", found}}}));
        measurement.set_setup_evidence (
          json::array ({{{"kind", "typedProbeEcho"},
                         {"source", "route_client_t.request_to_spot.async<PerfEchoReply>"},
                         {"observedValue", probes}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_request_streams (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        const auto &config = _role.config;
        const auto &spot_id =
          config.spot_ids[static_cast<std::size_t> (stream) % config.spot_ids.size ()];
        auto request = measurement.request (stream, _sequences.next (stream));
        std::int64_t started = 0;
        if (!measurement.begin_operation (started))
            co_return;
        request.sent_ticks = dec (started);
        std::exception_ptr error;
        try {
            const auto reply = co_await route.request_to_spot (spot_id, request)
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
