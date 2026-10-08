/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.4 s2s-channel-to-spot-send-send-echo. Question: how completion rate, throughput and round trip differ from the
// request form of §10.3 when both directions are one-way sends. Roles: HTTP Client x1, Channel process (Object Client plus
// the Server of a run/cell-only return ChannelName, this file) x1, Spot process (Object Server) x1.
// One operation: the correlation is registered and the first `route_client_t.send_to_spot(...).async()` starts; it ends when the
// return Channel handler validates the echo (§13). send-send, ordinary; payload 4096 bytes; `streamId mod spotCount` picks
// the Spot. Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.

#include <perf/server/spot_role.hpp>

namespace perf
{
class s2s_channel_to_spot_send_send_echo_scenario_t
{
  public:
    explicit s2s_channel_to_spot_send_send_echo_scenario_t (role_t &role) :
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
        auto &correlations = *_role.correlations;
        json probes = json::array ();
        for (std::size_t target = 0; target < config.spot_ids.size (); ++target) {
            auto request = measurement.request (static_cast<int> (target),
                                                _sequences.next (static_cast<int> (target)), true);
            request.return_channel = config.channel_name;
            const auto entry =
              correlations.register_request (request, parse_i64 (request.sent_ticks));
            route.send_to_spot (config.spot_ids[target], request).async ().result ().value ();
            const auto [error, completed] = correlations.complete (entry).result ().value ();
            (void) completed;
            if (error)
                std::rethrow_exception (error);
            probes.push_back ({{"correlationId", request.correlation_id}});
        }
        // objectsReady only after every probe, so warmup never overlaps one
        _role.objects->set (
          true, "",
          json::array (
            {{{"kind", "spotFind"}, {"source", "spot_manager_t.find"}, {"observedValue", found}}}));
        measurement.set_setup_evidence (
          json::array ({{{"kind", "typedProbeEcho"},
                         {"source", "route_client_t.send_to_spot -> return Channel send handler"},
                         {"observedValue", probes}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_stream_loops (loops, _role,
                            [this, loops] (int stream) { return loop (stream, loops); });
    }

  private:
    fw::task_t<void> loop (int stream, loops_t loops)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        auto &correlations = *_role.correlations;
        const auto &config = _role.config;
        const auto &spot_id =
          config.spot_ids[static_cast<std::size_t> (stream) % config.spot_ids.size ()];
        auto request = measurement.request (stream, _sequences.next (stream));
        request.return_channel = config.channel_name;
        std::int64_t started = 0;
        if (!measurement.begin_operation (started, "send"))
            co_return;
        request.sent_ticks = dec (started);
        const auto entry = correlations.register_request (
          request, started); // §13: registered right before the first public send
        try {
            co_await route.send_to_spot (spot_id, request).async ();
            correlations.first_send_ended (entry, nullptr);
        }
        catch (...) {
            correlations.first_send_ended (entry, std::current_exception ());
        }
        loops->spawn (observe_send_echo (_role, entry), [this] (std::exception_ptr error) {
            _role.measurement.record_diagnostic (std::move (error));
        });
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
