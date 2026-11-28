// PUBSUB multi server benchmark: one-way publisher source.
// Topology: server PUB(bind, 1) -> client SUB(connect, N)
// Measurement role: stamp payload phases and publish continuously.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include "../common/perf_multi_measurement.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

namespace
{

static const char *k_topic = "bench";
static volatile std::sig_atomic_t g_stop_requested = 0;

void on_signal (int)
{
    g_stop_requested = 1;
}

void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
}

bool wait_for_start_signal (size_t msg_size)
{
    return perf::multi::wait_for_start_from_stdin (msg_size);
}

// Publish the wire-level stop token on the active topic with a blocking
// publish so subscribers are woken from their -1 poller wait and learn the
// active phase has ended. Matches C reference publish_stop_token() in
// bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:113-144.
bool publish_stop_token (::perf::socket_t &publisher)
{
    const size_t token_size = std::strlen (perf::multi::k_stop_token);
    while (!g_stop_requested) {
        zlink::message_t part (token_size);
        if (!part.valid ())
            return false;
        std::memcpy (part.data (), perf::multi::k_stop_token, token_size);

        const int rc =
          publisher.publish (k_topic, part, static_cast<int> (zlink::send_flags_t::none));
        if (rc == 0)
            return true;

        const int err = errno;
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
            continue;
        return false;
    }

    return true;
}

bool run_phase (::perf::socket_t &publisher,
                size_t msg_size,
                uint32_t run_id,
                uint64_t &seq,
                perf_metric::phase_t phase,
                std::chrono::steady_clock::duration duration,
                bool send_active,
                uint64_t *active_sends = NULL,
                perf_multi_measurement::schedule_t *schedule = NULL,
                const std::atomic<uint64_t> *caught_up_ns = NULL)
{
    if (duration <= std::chrono::steady_clock::duration::zero ())
        return true;

    if (!send_active)
        return true;

    try {
        const size_t send_size = std::max<size_t> (static_cast<size_t> (1), msg_size);
        const auto duration_ns = static_cast<uint64_t> (
          std::chrono::duration_cast<std::chrono::nanoseconds> (duration).count ());
        const uint64_t deadline_ns = static_cast<uint64_t> (perf_metric::now_ns ()) + duration_ns;
        for (;;) {
            const uint64_t now_ns = static_cast<uint64_t> (perf_metric::now_ns ());
            const uint64_t caught_up =
              caught_up_ns ? caught_up_ns->load (std::memory_order_acquire) : 0;
            const uint64_t end_ns =
              caught_up != 0 ? std::min (deadline_ns, caught_up + duration_ns / 2) : deadline_ns;
            if (now_ns >= end_ns)
                break;
            if (schedule && !schedule->due (now_ns)) {
                std::this_thread::sleep_for (perf_multi_measurement::schedule_t::wait_ms (
                  now_ns, std::min (end_ns, schedule->next_ns ())));
                continue;
            }
            zlink::message_t payload_part (send_size);
            if (!payload_part.valid ())
                return false;
            // HOT PATH: construct the native payload that the public publish
            // operation consumes, then stamp its frame directly. Copying a
            // temporary vector into every message adds no wire-level value.
            const perf_metric::phase_t message_phase =
              schedule && caught_up == 0 ? perf_metric::phase_warmup : phase;
            if (!perf_metric::stamp_payload (payload_part.data (), send_size, run_id, message_phase,
                                             msg_size, seq++, perf_metric::now_ns ()))
                return false;
            int sent = -1;
            if (perf::multi::measurement_part_count () == 2) {
                zlink::message_t tail = perf::multi::measurement_empty_part ();
                sent = publisher.publish (
                  k_topic, payload_part, tail,
                  static_cast<int> (zlink::send_flags_t::none));
            } else {
                sent = publisher.publish (
                  k_topic, payload_part,
                  static_cast<int> (zlink::send_flags_t::none));
            }
            if (sent != 0)
                return false;
            if (schedule)
                schedule->accepted (static_cast<uint64_t> (perf_metric::now_ns ()));
            else if (active_sends)
                ++*active_sends;
        }

        return true;
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
}

} // namespace

bool perf_pubsub_server (const std::string &lib_name, const std::string &transport, size_t msg_size)
{
    perf::multi::set_perf_pattern_env ("PUBSUB");

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",PUBSUB," << transport << std::endl;
        return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    perf::multi::ctx_guard_t ctx;
    perf::multi::socket_guard_t publisher (ctx, zlink::socket_type::pub);
    if (!publisher.valid ())
        return false;

    perf::multi::apply_benchmark_socket_options (publisher.sock (), settings, transport);
    if (!perf::multi::setup_tls_server (publisher.sock (), transport))
        return false;

    const std::string endpoint = perf::multi::bind_and_resolve_endpoint (
      publisher.sock (), transport, "cpp_multi_pubsub", settings.server_bind_port);
    if (endpoint.empty ())
        return false;

    perf::multi::print_ready (endpoint);

    if (!wait_for_start_signal (msg_size)) {
        std::cerr << "PUBSUB_SERVER_FAIL,stage=start_signal,transport=" << transport
                  << ",size=" << msg_size << std::endl;
        return false;
    }

    perf::multi::apply_benchmark_hwm (publisher.sock (), settings.hwm, settings.hwm);
    if (!perf::multi::recalculate_auto_hwm (ctx))
        return false;
    perf::multi::emit_auto_hwm_detail (publisher.sock (), "server", "server", transport, msg_size,
                                       "pub");

    const uint32_t run_id = 1U;
    uint64_t seq = 1;
    uint64_t active_sends = 0;

    if (!run_phase (publisher.sock (), msg_size, run_id, seq, perf_metric::phase_active,
                    std::chrono::seconds (std::max (1, settings.duration_seconds)), true,
                    &active_sends))
        return false;

    // Signal active-phase end via the wire-level stop token on the active
    // topic (blocking publish, deadline ignored), matching the C reference
    // (bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:265-268).
    if (!publish_stop_token (publisher.sock ()))
        return false;

    std::cout << "PHASE_DONE," << msg_size << ',' << static_cast<int> (perf_metric::phase_active)
              << std::endl;
    size_t active_received = 0;
    if (!perf::multi::wait_for_size_count_from_stdin (msg_size, "PHASE_PACED,", &active_received))
        return false;
    const uint64_t epoch = static_cast<uint64_t> (perf_metric::now_ns ());
    perf_multi_measurement::schedule_t schedule (
      perf_multi_measurement::target_rate (active_received,
                                           static_cast<double> (settings.duration_seconds)),
      epoch);
    if (schedule.rate <= 0.0)
        return false;
    std::atomic<uint64_t> caught_up_ns (0);
    std::atomic<uint64_t> paced_received (0);
    std::thread phase_input ([&] {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t size = 0;
            size_t count = 0;
            if (perf::multi::parse_size_command_line (line, "PHASE_CATCHUP,", &size)
                && size == msg_size) {
                const uint64_t now = static_cast<uint64_t> (perf_metric::now_ns ());
                if (now <= epoch
                             + static_cast<uint64_t> (settings.latency_duration_seconds)
                                 * 1000000000ULL)
                    caught_up_ns.store (now, std::memory_order_release);
            } else if (perf::multi::parse_size_count_command_line (line, "PHASE_COMPLETE,", &size,
                                                                   &count, true)
                       && size == msg_size) {
                paced_received.store (count, std::memory_order_release);
                break;
            } else if (line == "STOP" || line == "QUIT")
                break;
        }
    });
    const bool paced_ok =
      run_phase (publisher.sock (), msg_size, run_id, seq, perf_metric::phase_latency,
                 std::chrono::seconds (2 * settings.latency_duration_seconds), true, NULL,
                 &schedule, &caught_up_ns);
    const bool stop_ok = paced_ok && publish_stop_token (publisher.sock ());
    std::cout << "PHASE_DONE," << msg_size << ',' << static_cast<int> (perf_metric::phase_latency)
              << std::endl;
    phase_input.join ();
    perf_multi_measurement::print_diagnostic (
      "PUBSUB", msg_size, active_sends, settings.duration_seconds, schedule, active_received,
      paced_received.load () / static_cast<double> (settings.latency_duration_seconds),
      caught_up_ns.load () != 0,
      caught_up_ns.load () != 0 ? (caught_up_ns.load () - epoch) / 1e9 : -1.0);
    std::cout << "REFERENCE," << lib_name << ",PUBSUB," << transport << ',' << msg_size
              << ",sender_late," << schedule.late_count << std::endl;
    return paced_ok && stop_ok;
}

int main (int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: <lib_name> <transport> <size>" << std::endl;
        return 1;
    }

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));
    if (size == 0)
        return 1;

    install_signal_handlers ();
    return perf_pubsub_server (lib_name, transport, size) ? 0 : 1;
}
