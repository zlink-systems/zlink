/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.6 s2s-spot-to-channel-send-send-echo. Question: what a send that goes from a Spot to a Channel and returns to the
// original Spot's separate send handler costs. Roles: HTTP Client x1, Spot (Object Server + driver, this file) x1, Channel
// (Object Client) x1. The driver sends PerfDriveRequest to the Spot with `route_client_t.request_to_spot`; the Spot handler
// registers the correlation, makes the first `send_to_channel` and returns once that send is admitted, so the turn is free when
// the Channel's send comes back to the Spot's return handler. The driver, outside the turn, waits for the correlation and
// keeps the in-flight slot until the echo is validated (§13). One operation: correlation registration / first send -> return
// handler echo validation. The DTO's returnSpotId names the source User SpotId. send-send; ordinary; payload 4096 bytes.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.

#include <perf/server/spot_role.hpp>

namespace perf
{
class s2s_send_send_spot_t final : public perf_spot_base_t<s2s_send_send_spot_t>
{
  public:
    s2s_send_send_spot_t (fw::spot_context_t context, role_t &role, fw::route_client_t &route) :
        perf_spot_base_t (std::move (context)), _role (role), _route (route)
    {
    }
    void configure () override
    {
        _context.handlers ()
          .add_handler<&s2s_send_send_spot_t::drive> (drive_request_t::packet_name)
          .add_handler<&s2s_send_send_spot_t::on_return> (echo_reply_t::packet_name);
    }

    // The source Spot handler: register the correlation, make the first public send and return when it is admitted.
    fw::task_t<drive_reply_t> drive (const drive_request_t &drive)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const handler_scope_t scope (measurement);
        std::exception_ptr failure;
        bool probe = false;
        std::int64_t started = 0;
        try {
            auto request = drive.echo;
            measurement.validate_request (request, std::nullopt, _context.spot_id ());
            if (request.phase == "measured")
                _role.metrics.count ("spot.applicationHandlerEntries");
            probe = measurement.phase () == "setup"; // the setup probe is no measured operation
            started = now_ticks ();
            if (!probe && !measurement.begin_operation (started, "send"))
                co_return drive_reply_t{false, std::nullopt};
            request.sent_ticks = dec (started);
            // §13: register immediately before the first public send.
            const auto entry = _role.correlations->register_request (request);
            try {
                co_await _route.send_to_channel (*config.channel_name, request).async ();
                _role.correlations->first_send_ended (entry, nullptr);
            }
            catch (...) {
                _role.correlations->first_send_ended (entry, std::current_exception ());
            }
        }
        catch (...) {
            failure = std::current_exception ();
        }
        if (failure) {
            // The operation started but cannot be tied to a correlation: it ends here as a failure.
            if (!probe && started != 0)
                measurement.complete_operation (started, failure);
            std::rethrow_exception (failure);
        }
        co_return drive_reply_t{true, std::nullopt}; // send/send: the reply is only the first send's acknowledgement
    }

    // The return send arrives as its own Spot packet: the correlation decides the operation's first result.
    fw::task_t<void> on_return (const echo_reply_t &message)
    {
        const handler_scope_t scope (_role.measurement);
        if (message.phase == "measured")
            _role.metrics.count ("spot.applicationHandlerEntries");
        _role.correlations->reply (message);
        co_return;
    }

  private:
    role_t &_role;
    fw::route_client_t &_route;
};

class s2s_spot_to_channel_send_send_echo_scenario_t
{
  public:
    explicit s2s_spot_to_channel_send_send_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        role.metrics.counters ({"driver.issued", "driver.notStarted", "driver.failed", "spot.applicationHandlerEntries"})
          .latency ("driverLatencyMs", "driver.latency")
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
            auto echo = measurement.request (static_cast<int> (target), _sequences.next (static_cast<int> (target)), true);
            echo.return_spot_id = config.spot_ids[target];
            const auto driven = route.request_to_spot (config.spot_ids[target], drive_request_t{echo})
                                  .timeout (std::chrono::milliseconds (config.workload.driver_timeout_ms))
                                  .async<drive_reply_t> ()
                                  .result ()
                                  .value ();
            if (!driven.started)
                throw validation_error_t ("IdentityMismatch", "The setup probe was not started.");
            const auto entry = _role.correlations->find (echo.correlation_id);
            if (!entry)
                throw validation_error_t ("UnknownCorrelation", "The setup probe registered no correlation.");
            const auto [error, completed] = _role.correlations->complete (entry).result ().value ();
            (void) completed;
            if (error)
                std::rethrow_exception (error);
            probes.push_back ({{"correlationId", echo.correlation_id}});
        }
        publish_spots (_role, objects); // objectsReady only after every probe, so warmup never overlaps one
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"},
                                                       {"source", "Spot send_to_channel -> Channel send_to_spot -> Spot return handler"},
                                                       {"observedValue", probes}}}));
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
            auto echo = measurement.request (stream, _sequences.next (stream));
            echo.return_spot_id = spot_id;
            const auto driver_started = now_ticks ();
            _role.metrics.count ("driver.issued");
            std::exception_ptr error;
            try {
                drive_reply_t driven;
                try {
                    driven = co_await route.request_to_spot (spot_id, drive_request_t{echo})
                               .timeout (std::chrono::milliseconds (config.workload.driver_timeout_ms)).async<drive_reply_t> ();
                }
                catch (...) {
                    _role.metrics.count ("driver.failed");
                    throw;
                }
                const auto driver_finished = now_ticks ();
                if (!driven.started) {
                    _role.metrics.count ("driver.notStarted");
                    continue;
                }
                // Outside the Spot turn: the final result of the correlation the handler registered (§13).
                const auto entry = _role.correlations->find (echo.correlation_id);
                if (!entry)
                    throw validation_error_t ("UnknownCorrelation", "The started drive registered no correlation.");
                const auto [result, completed] = co_await _role.correlations->complete (entry);
                if (measurement.complete_operation (parse_i64 (entry->request.sent_ticks), result, completed))
                    _role.metrics.record ("driverLatencyMs", driver_started, driver_finished);
            }
            catch (...) {
                error = std::current_exception ();
            }
            if (error) {
                measurement.record_diagnostic (error);
            }
        }
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
