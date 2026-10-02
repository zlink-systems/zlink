/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/streams/session_liveness.hpp"
#include "runtime/dispatch/application_job_queue.hpp"
#include <zlink/framework/contracts/streams/stream.hpp>
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
using zlink::framework::runtime::session_liveness_t;
int main ()
{
    using decision = session_liveness_t::decision_t;
    const auto established = session_liveness_t::clock_t::time_point{};
    session_liveness_t silent (established);
    silent.record_inbound (established - 1s);
    if (silent.evaluate (established + 4999ms) == decision::heartbeat_timeout)
        return 1;
    if (silent.evaluate (established + 5s) != decision::heartbeat_timeout) {
        std::cerr << "FAIL a: first inbound absent at 5s must heartbeat_timeout\n";
        return 1;
    }
    std::cout << "PASS a: first inbound absent heartbeat_timeout\n";
    for (const bool core_transport : {false, true}) {
        for (const bool application_data : {false, true}) {
            session_liveness_t active (established);
            for (int cycle = 1; cycle <= 5; ++cycle) {
                const auto received = established + cycle * 4999ms;
                active.record_inbound (received, application_data);
                if (active.evaluate (received + 1ms) == decision::heartbeat_timeout) {
                    std::cerr << "FAIL b: inbound must refresh heartbeat\n";
                    return 1;
                }
                if (application_data && active.last_application_inbound != received)
                    return 1;
            }
            std::cout << "PASS b: " << (core_transport ? "Core " : "raw ")
                      << (application_data ? "data" : "pong") << " inbound refreshes heartbeat\n";
        }
    }
    session_liveness_t delayed_factory (established, established + 10s);
    delayed_factory.record_inbound (established + 1s);
    if (delayed_factory.evaluate (established + 6s) != decision::heartbeat_timeout)
        return 1;
    session_liveness_t core_data_idle (established);
    core_data_idle.record_inbound (established + 30s, true);
    if (core_data_idle.evaluate (established + 30s) == decision::idle_timeout)
        return 1;
    session_liveness_t raw_data_idle (established);
    raw_data_idle.record_inbound (established + 30s, true);
    if (raw_data_idle.evaluate (established + 30s) == decision::idle_timeout
        || raw_data_idle.last_application_inbound != established + 30s)
        return 1;
    session_liveness_t periodic (established);
    for (int tick = 1; tick < 5; ++tick) {
        if (periodic.evaluate (established + tick * 1s) != decision::send_heartbeat) {
            std::cerr << "FAIL: periodic ping must not wait for pong\n";
            return 1;
        }
    }
    if (periodic.evaluate (established + 5s) != decision::heartbeat_timeout)
        return 1;
    session_liveness_t controls (established);
    controls.record_inbound (established + 30s);
    if (controls.evaluate (established + 30s) != decision::idle_timeout)
        return 1;
    session_liveness_t both_expired (established);
    if (both_expired.evaluate (established + 30s) != decision::heartbeat_timeout)
        return 1;
    // Producers submit timestamps; the connection execution owner alone
    // mutates and evaluates the policy, including reordered handler entries.
    boost::asio::io_context io;
    session_liveness_t owned (established);
    std::thread handler ([&] {
        boost::asio::post (io, [&] { owned.record_application_inbound (established + 20s); });
        boost::asio::post (io, [&] { owned.record_application_inbound (established + 10s); });
        boost::asio::post (io, [&] { owned.record_inbound (established + 30s); });
    });
    handler.join ();
    if (owned.next_due () != established + 1s)
        return 1;
    io.poll ();
    if (owned.evaluate (established + 30s) == decision::idle_timeout
        || owned.last_application_inbound != established + 20s)
        return 1;
    owned.record_inbound (established + 30s, true);
    owned.record_inbound (established + 29s, true);
    if (owned.last_application_inbound != established + 30s)
        return 1;
    io.restart ();
    bool owner_valid = true;
    boost::asio::post (io, [&] {
        if (!owned.try_terminate (zlink::framework::stream_close_reason_t::server_drain))
            owner_valid = false;
    });
    boost::asio::post (io, [&] {
        owned.record_inbound (established + 60s);
        if (owned.evaluate (established + 90s) != decision::none
            || owned.next_due () != session_liveness_t::clock_t::time_point::max ())
            owner_valid = false;
    });
    io.poll ();
    if (!owner_valid)
        return 1;

    // Capacity shutdown publishes a wake; blocking run_one needs no polling.
    using jobs_t = zlink::framework::runtime::application_job_queue_t;
    jobs_t jobs (zlink::framework::runtime::application_job_queue_configuration_t{});
    jobs_t::supply_request_t held_supply;
    auto held = held_supply.take (jobs, 0ms);
    if (!held || !*held)
        return 1;
    jobs_t::supply_request_t pending_supply;
    io.restart ();
    bool woke = false;
    if (pending_supply.take (jobs, 0ms, [&] { boost::asio::post (io, [&] { woke = true; }); }))
        return 1;
    const auto keep_alive = boost::asio::make_work_guard (io);
    std::thread stopper ([&] { jobs.stop (); });
    stopper.join ();
    boost::asio::steady_timer missing_wake_bound (io, 2s);
    missing_wake_bound.async_wait ([] (const boost::system::error_code &) {});
    io.run_one ();
    missing_wake_bound.cancel ();
    if (!woke) {
        std::cerr << "FAIL: supply stop did not wake the blocking io owner\n";
        return 1;
    }
    const auto stopped = pending_supply.take (jobs, 0ms);
    if (!stopped || *stopped)
        return 1;
    std::cout << "PASS owner submission, monotonic handler-entry and force/stop wake\n";
    std::cout << "PASS periodic ping and unchanged idle precedence\n";
}
