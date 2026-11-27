/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/stream_liveness_fixture.hpp"

namespace
{
using namespace stream_liveness_test;

void check_liveness (std::string_view transport,
                     std::string_view scenario,
                     std::chrono::milliseconds idle_timeout,
                     const detail::stream_runtime_t &runtime,
                     const std::string &endpoint,
                     session_callbacks_t &session,
                     rt::application_job_queue_t &jobs,
                     metric_test::provider_t &metrics)
{
    std::cout << "BEGIN " << transport << ' ' << scenario << std::endl;

    const bool saturate = scenario != "unsaturated";
    const bool silent_after_release = scenario == "silent_after_release";
    const auto timeout_after_release = silent_after_release ? receive_timeout : idle_timeout;
    const auto expected_reason = idle_timeout > 0ms ? fw::stream_close_reason_t::idle_timeout
                                                    : fw::stream_close_reason_t::heartbeat_timeout;
    const auto expected_reason_label = idle_timeout > 0ms ? "idle_timeout" : "heartbeat_timeout";
    const auto duration = scenario == "below_timeout" ? 300ms
                          : scenario == "at_timeout"  ? receive_timeout
                                                      : 1500ms;
    const bool core = transport == "core";
    // An incomplete Asio frame starts the permit wait and leaves the inbound
    // clock dependent on the permit release, without a decoded frame to refresh it.
    const bool partial_ping = silent_after_release && !core;
    ping_only_client_t client (endpoint, runtime);
    const auto &peer = client.observation ();
    client.send_ping ();
    const auto warmup_deadline = steady_clock::now () + 750ms;
    while ((peer.pongs == 0 || session.connections == 0) && !peer.closed
           && steady_clock::now () < warmup_deadline)
        client.advance ();
    require (peer.pongs == 1 && peer.writes == 1 && session.connections == 1,
             "Warm-up ping did not establish a responding STREAM session");

    std::optional<rt::application_job_queue_t::permit_t> held_permit;
    if (saturate) {
        // Separate the last decoded ping from the start of the hold so that
        // the 1,000 ms case does not tie the old timeout with permit release.
        const auto quiet_until = steady_clock::now () + 100ms;
        while (!peer.closed && steady_clock::now () < quiet_until)
            client.advance ();
        require (!peer.closed && !peer.closing, "The session closed before the permit hold");

        const auto reservation_deadline = steady_clock::now () + 100ms;
        while (!(held_permit = jobs.try_reserve_supply ())
               && steady_clock::now () < reservation_deadline)
            client.advance ();
        require (held_permit && jobs.snapshot ().permits_in_use == 1 && !jobs.try_reserve_supply (),
                 "Test could not occupy the only application permit");
    }
    const auto start = steady_clock::now ();
    auto server_pings_at_wait = peer.server_pings;
    std::optional<steady_clock::time_point> reader_waiting;
    auto next_ping = start;
    const auto drive_client = [&] {
        const auto now = steady_clock::now ();
        if (now >= next_ping && (!silent_after_release || peer.writes == 1)) {
            client.send_ping (partial_ping);
            next_ping = now + probe_period;
        }
        if (peer.closed)
            std::this_thread::sleep_for (2ms);
        else
            client.advance ();
    };
    while (steady_clock::now () < start + duration) {
        if (saturate && !reader_waiting && jobs.snapshot ().capacity_waiters == 1) {
            reader_waiting = steady_clock::now ();
            server_pings_at_wait = peer.server_pings;
        }
        drive_client ();
    }
    if (saturate) {
        const auto write_deadline = steady_clock::now () + probe_period;
        while (client.write_pending () && steady_clock::now () < write_deadline)
            client.advance ();
        require (!client.write_pending (), "Client ping write was still pending at release");
    }

    const auto at_release = peer;
    if (saturate)
        require (jobs.snapshot ().permits_in_use == 1, "Test lost the permit before release");
    const auto released = steady_clock::now ();
    held_permit.reset ();
    const auto post_release_window = timeout_after_release + observation_after_release;
    while (!peer.closed && steady_clock::now () < released + post_release_window) {
        if (saturate)
            client.advance ();
        else
            drive_client ();
    }

    if (peer.closed) {
        const auto cleanup_deadline = steady_clock::now () + observation_after_release;
        while (session.disconnections == 0 && steady_clock::now () < cleanup_deadline)
            std::this_thread::sleep_for (2ms);
        require (session.disconnections == 1,
                 "Closed session did not finish its disconnect callback");
    }

    std::optional<std::string> server_reason;
    for (const auto &metric : metrics.collect ()) {
        if (metric.instrument_descriptor.name_ != "zlink.stream.connections.closed")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            const auto &sum =
              opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
            require (!server_reason && opentelemetry::nostd::get<double> (sum.value_) == 1.0,
                     "Expected a close metric for exactly one session");
            server_reason =
              opentelemetry::nostd::get<std::string> (point.attributes.at ("close_reason"));
        }
    }

    const auto elapsed = [start] (std::optional<steady_clock::time_point> time) {
        return time ? std::chrono::duration_cast<std::chrono::milliseconds> (*time - start).count ()
                    : -1;
    };
    const auto pongs_after_release = peer.pongs - at_release.pongs;
    std::cout << "OBSERVATION " << transport << ' ' << scenario
              << " target_ms=" << duration.count () << " release_ms=" << elapsed (released)
              << " first_waiter_ms=" << elapsed (reader_waiting)
              << " pings_in_window=" << at_release.writes - 1
              << " last_ping_in_window_ms=" << elapsed (at_release.last_write)
              << " pongs_in_window=" << at_release.pongs - 1
              << " pongs_after_release=" << pongs_after_release
              << " server_pings_in_window=" << at_release.server_pings - server_pings_at_wait
              << " closing_ms=" << elapsed (peer.closing) << " closed_ms=" << elapsed (peer.closed)
              << " close_after_release_ms="
              << (peer.closed ? elapsed (peer.closing.value_or (*peer.closed)) - elapsed (released)
                              : -1)
              << " close_observed_before_release=" << at_release.closed.has_value ()
              << " server_close_reason=" << server_reason.value_or ("none") << " wire_close_reason="
              << (peer.reason == expected_reason ? expected_reason_label
                  : peer.reason                  ? "unexpected"
                                                 : "none")
              << " read_error=" << peer.read_error.message ()
              << " write_error=" << peer.write_error.message () << std::endl;

    require (session.errors == 0 && session.packets == 0 && session.connections == 1,
             "Unexpected session callback during the ping-only case");
    if (saturate) {
        require (reader_waiting.has_value (), "Reader did not contend for the application permit");
        require (at_release.writes > 1 && at_release.pongs == 1,
                 "Expected successful ping writes without pongs while the permit was held");
        require (!at_release.closed && !at_release.closing,
                 "The session closed while the reader was waiting for the application permit");
        require (at_release.server_pings - server_pings_at_wait >= std::max (
                   decltype (duration / server_ping_period){1}, duration / server_ping_period - 1),
                 "The server did not keep sending heartbeat pings during the permit hold");
        const auto expected_pongs = partial_ping ? 0 : at_release.writes - at_release.pongs;
        require (pongs_after_release == expected_pongs,
                 "The server did not answer the pings queued during the permit hold");
    }

    const bool alive = !peer.closing && !peer.closed;
    if (alive) {
        require (!server_reason && !peer.write_error && pongs_after_release > 0,
                 "The session stayed open without answering pings after release");
    } else {
        require (peer.closed && server_reason == expected_reason_label,
                 "The server did not record the expected timeout reason");
        require (!peer.reason || peer.reason == expected_reason,
                 "Session-closing reported a different reason from the server metric");
        require (peer.read_error == asio::error::eof
                   || peer.read_error == asio::error::connection_reset
                   || peer.read_error == websocket::error::closed,
                 "Client did not observe an expected transport close");
    }
    if (!saturate) {
        require (alive && at_release.pongs > 10, "The unsaturated control case did not stay live");
    } else if (timeout_after_release > 0ms) {
        if (silent_after_release)
            require (peer.writes == 2, "The silent peer must write exactly once during the hold");
        require (peer.closed
                   && peer.closing.value_or (*peer.closed) >= released + timeout_after_release
                   && *peer.closed <= released + post_release_window,
                 "The session did not close one full timeout after release");
    } else {
        require (alive, "The session did not stay live through the permit hold and release");
    }
}
}

int main (int argc, char **argv)
{
    try {
        const fw::completion_owner_t completion_owner;
        detail::configure_stream_dispatch_executor ();
        struct dispatch_executor_guard_t
        {
            ~dispatch_executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
        } dispatch_executor_guard;
        require (argc == 3, "Expected a transport and a scenario");
        const std::string_view transport = argv[1];
        const std::string_view scenario = argv[2];
        const auto idle_timeout = scenario == "idle_after_release" ? 450ms : 0ms;
        with_stream_host (transport, idle_timeout,
                          [&] (auto &, const auto &runtime, const auto &endpoint, auto &session,
                               auto &jobs, auto &metrics) {
                              check_liveness (transport, scenario, idle_timeout, runtime, endpoint,
                                              session, jobs, metrics);
                          });
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << std::endl;
        return 1;
    }
}
