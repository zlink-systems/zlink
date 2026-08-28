// PUBSUB multi server benchmark: one-way publisher source.
// Topology: server PUB(bind, 1) -> client SUB(connect, N)
// Measurement role: stamp payload phases and publish continuously.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
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
                bool send_active)
{
    if (duration <= std::chrono::steady_clock::duration::zero ())
        return true;

    if (!send_active)
        return true;

    try {
        const size_t send_size = std::max<size_t> (static_cast<size_t> (1), msg_size);
        const auto deadline = std::chrono::steady_clock::now () + duration;
        while (std::chrono::steady_clock::now () < deadline) {
            zlink::message_t payload_part (send_size);
            if (!payload_part.valid ())
                return false;
            // HOT PATH: construct the native payload that the public publish
            // operation consumes, then stamp its frame directly. Copying a
            // temporary vector into every message adds no wire-level value.
            if (!perf_metric::stamp_payload (payload_part.data (), send_size,
                                             run_id, phase, msg_size, seq++,
                                             perf_metric::now_ns ()))
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
        std::cout << "UNSUPPORTED," << lib_name << ",MULTI_PUBSUB," << transport << std::endl;
        return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    perf::multi::ctx_guard_t ctx;
    perf::multi::socket_guard_t publisher (ctx, zlink::socket_type::pub);
    if (!publisher.valid ())
        return false;

    perf::multi::apply_benchmark_socket_options (publisher.sock (), settings, transport);
    if (!perf::multi::recalculate_auto_hwm (ctx))
        return false;
    if (!perf::multi::setup_tls_server (publisher.sock (), transport))
        return false;

    const std::string endpoint = perf::multi::bind_and_resolve_endpoint (
      publisher.sock (), transport, "cpp_multi_pubsub", settings.server_bind_port);
    if (endpoint.empty ())
        return false;
    perf::multi::emit_auto_hwm_detail (publisher.sock (), "server", "server", transport, msg_size,
                                       "pub");

    perf::multi::print_ready (endpoint);

    if (!wait_for_start_signal (msg_size)) {
        std::cerr << "PUBSUB_SERVER_FAIL,stage=start_signal,transport=" << transport
                  << ",size=" << msg_size << std::endl;
        return false;
    }

    const uint32_t run_id = 1U;
    uint64_t seq = 1;

    if (!run_phase (publisher.sock (), msg_size, run_id, seq,
                    perf_metric::phase_active,
                    std::chrono::seconds (std::max (1, settings.duration_seconds)), true))
        return false;

    // Signal active-phase end via the wire-level stop token on the active
    // topic (blocking publish, deadline ignored), matching the C reference
    // (bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:265-268).
    if (!publish_stop_token (publisher.sock ()))
        return false;

    return true;
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
