/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.5 s2s-spot-to-channel-request-echo. Question: how the terminal (ordinary or Yield) and the number of Spots change
// completion throughput, tail latency and the progress of other callbacks for the same Spot -> Channel remote request.
// Roles: HTTP Client x1, Spot process (Object Server + local public driver, this file) x1, Channel echo target x1. The driver
// sends PerfDriveRequest to the Spot with `route_client_t.request_to_spot`; the Spot handler (an Actor-less SpotWide User
// Spot) makes one `route_client_t.request_to_channel`. The measured operation starts right before that remote call and ends
// after the reply is validated (after the turn is regained); driver time is kept apart as driver.latency.*. Streams are
// assigned to SpotIds round-robin, so no Actor queue takes part. request; ordinary or yield x 1 or 16 Spots; payload 4096
// bytes. Store: run Docker Redis (Spot addresses and automatic mesh). Null: physical connections, worker, Actor, fanout;
// suspended/resumed turns, resume latency and mailbox depth have no public observation. Yield calls are the application's
// calls, not proven turn suspensions.

#include <perf/server/spot_role.hpp>

namespace perf
{
// The Spot handler: one request_to_channel per drive request. Ordinary keeps the Spot turn until the reply; Yield hands the
// turn back while the reply is pending (execution gate contract). This is the measured operation.
class s2s_remote_request_spot_t final : public perf_spot_base_t<s2s_remote_request_spot_t>
{
  public:
    s2s_remote_request_spot_t (fw::spot_context_t context, role_t &role, fw::route_client_t &route) :
        perf_spot_base_t (std::move (context)), _role (role), _route (route)
    {
    }
    void configure () override { _context.handlers ().add_handler<&s2s_remote_request_spot_t::drive> (drive_request_t::packet_name); }

    fw::task_t<drive_reply_t> drive (const drive_request_t &drive)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const handler_scope_t scope (measurement);
        drive_reply_t out;
        std::exception_ptr failure;
        bool probe = false;
        bool operation_started = false;
        std::int64_t started = 0;
        try {
            auto request = drive.echo;
            measurement.validate_request (request);
            if (request.phase == "measured")
                _role.metrics.count ("spot.applicationHandlerEntries");
            probe = measurement.phase () == "setup"; // the setup probe is no measured operation
            started = now_ticks ();
            if (!probe && !measurement.begin_operation (started)) {
                co_return drive_reply_t{false, std::nullopt};
            }
            operation_started = !probe;
            request.sent_ticks = dec (started);
            auto call = _route.request_to_channel (*config.channel_name, request)
                          .timeout (measurement.call_timeout ());
            echo_reply_t reply;
            if (config.terminal == "yield") {
                _role.metrics.count ("spot.applicationYieldCalls");
                reply = co_await call.yield<echo_reply_t> ();
            }
            else
                reply = co_await call.async<echo_reply_t> ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
            if (!probe)
                measurement.complete_operation (started);
            out.started = true;
            out.echo = reply;
        }
        catch (...) {
            failure = std::current_exception ();
        }
        if (failure) {
            if (probe)
                std::rethrow_exception (failure);
            if (!operation_started) {
                measurement.record_diagnostic (failure);
                std::rethrow_exception (failure);
            }
            measurement.complete_operation (started, failure);
            out.started = true;
            out.echo = std::nullopt;
        }
        co_return out;
    }

  private:
    role_t &_role;
    fw::route_client_t &_route;
};

class s2s_spot_to_channel_request_echo_scenario_t
{
  public:
    explicit s2s_spot_to_channel_request_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        role.metrics.counters ({"driver.issued", "driver.notStarted", "driver.failed", "spot.applicationHandlerEntries", "spot.applicationYieldCalls"})
          .latency ("driverLatencyMs", "driver.latency")
          .alias_latency ("latency", "spot.remoteCallLatency")
          .spot_internals_unsupported ();
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto objects = create_spots (_role, stopping);
        wait_for_public (_role, stopping, [&] {
            const auto status = _role.mesh.load ()->snapshot (*config.mesh_name);
            return std::any_of (status.channels.begin (), status.channels.end (), [&] (const auto &c) {
                return c.channel_name == config.channel_name && c.is_ready && c.ready_target_count > 0;
            });
        }, "a ready Channel target");
        auto &route = _role.service<fw::route_client_t> ();
        json probes = json::array ();
        for (std::size_t target = 0; target < config.spot_ids.size (); ++target) {
            const auto echo = measurement.request (static_cast<int> (target), _sequences.next (static_cast<int> (target)), true);
            const auto driven = route.request_to_spot (config.spot_ids[target], drive_request_t{echo})
                                  .timeout (measurement.call_timeout (true))
                                  .async<drive_reply_t> ()
                                  .result ()
                                  .value ();
            if (!driven.echo)
                throw validation_error_t ("IdentityMismatch", "The setup probe did not reach the Channel.");
            probes.push_back ({{"correlationId", echo.correlation_id}, {"receivedTicks", driven.echo->received_ticks}, {"clockDomainId", driven.echo->clock_domain_id}});
        }
        publish_spots (_role, objects); // objectsReady only after every probe, so warmup never overlaps one
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "route_client_t.request_to_spot -> Spot request_to_channel"}, {"observedValue", probes}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_request_streams (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    // The local driver submits each PerfDriveRequest without waiting for an earlier drive reply.
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        const auto &config = _role.config;
        const auto &spot_id = config.spot_ids[static_cast<std::size_t> (stream) % config.spot_ids.size ()];
        const auto echo = measurement.request (stream, _sequences.next (stream));
        const auto started = now_ticks ();
        _role.metrics.count ("driver.issued");
        std::exception_ptr error;
        try {
            drive_reply_t driven;
            try {
                driven = co_await route.request_to_spot (spot_id, drive_request_t{echo})
                           .timeout (measurement.call_timeout (true)).async<drive_reply_t> ();
            }
            catch (...) {
                _role.metrics.count ("driver.failed");
                throw;
            }
            const auto driver_finished = now_ticks ();
            if (!driven.started)
                _role.metrics.count ("driver.notStarted");
            else if (driven.echo)
                _role.metrics.record ("driverLatencyMs", started, driver_finished);
        }
        catch (...) {
            error = std::current_exception ();
        }
        if (error) {
            measurement.record_diagnostic (error);
        }
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
