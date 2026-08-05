// DEALER-DEALER multi server benchmark: one-way receive sink.
// Topology: client DEALER(connect, N) -> server DEALER(bind, 1)
// Measurement role: drain incoming payloads and emit server queue metrics.

#include "../common/perf_common.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_metric_header.hpp"

#include <algorithm>
#include <any>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <vector>

namespace
{

void apply_dealer_socket_options (zlink::dealer_socket_t &socket,
                                  const perf::multi::multi_bench_settings_t &settings)
{
    zlink::dealer_socket_options_t options = socket.options ();
    if (perf::multi::manual_socket_overrides_enabled ()) {
        options.send_hwm (
          zlink::byte_count_t::bytes (
            static_cast<uint64_t> (settings.sndhwm > 0 ? settings.sndhwm : 1)));
        options.recv_hwm (
          zlink::byte_count_t::bytes (
            static_cast<uint64_t> (settings.rcvhwm > 0 ? settings.rcvhwm : 1)));
    }
    options.send_timeout (std::chrono::milliseconds (settings.sndtimeo_ms));
    options.recv_timeout (std::chrono::milliseconds (settings.rcvtimeo_ms));
    options.linger (std::chrono::milliseconds (0));
}

std::string
bind_dealer_endpoint (zlink::dealer_socket_t &socket, const std::string &transport, int fixed_port)
{
    const std::string bind_endpoint =
      perf::multi::make_endpoint (transport, "cpp_multi_dealer_dealer", fixed_port);
    try {
        socket.bind (bind_endpoint);
    }
    catch (const zlink::binding_error_t &) {
        return std::string ();
    }
    return transport == "inproc"
             ? bind_endpoint
             : perf::multi::normalize_endpoint_host (socket.options ().last_endpoint ());
}

} // namespace

bool perf_dealer_dealer_server (const std::string &lib_name,
                                const std::string &transport,
                                size_t msg_size)
{
    perf::multi::set_perf_pattern_env ("DEALER_DEALER");

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << ",MULTI_DEALER_DEALER," << transport
                  << std::endl;
        return true;
    }

    try {
        const perf::multi::multi_bench_settings_t settings =
          perf::multi::resolve_multi_bench_settings ();

        perf::multi::ctx_guard_t ctx;
        zlink::dealer_socket_t server (ctx.ctx ());
        if (!server.valid ())
            return false;

        apply_dealer_socket_options (server, settings);
        if (!perf::multi::setup_tls_server (server, transport))
            return false;

        zlink::poller_t poller;
        poller.add (server, zlink::poll_event_flag_t::pollin, 0);

        const std::string endpoint =
          bind_dealer_endpoint (server, transport, settings.server_bind_port);
        if (endpoint.empty ())
            return false;

        perf::multi::print_ready (endpoint);

        if (!perf::multi::wait_for_start_from_stdin (msg_size))
            return false;
        if (!perf::multi::apply_benchmark_auto_hwm_msg_unit (ctx, msg_size)
            || !perf::multi::recalculate_auto_hwm (ctx))
            return false;
        perf::multi::emit_auto_hwm_detail (server, "server", "server", transport, msg_size,
                                           "dealer");

        const int active_seconds = settings.duration_seconds > 0 ? settings.duration_seconds : 1;
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (active_seconds);

        bool failed = false;
        unsigned long long active_count = 0;
        perf::multi::bench_latency_sampler_t latency (static_cast<size_t> (active_seconds)
                                                      * 5000000U);
        zlink::message_t part;
        std::vector<zlink::poll_event_t> events (1);
        size_t stop_count = 0;
        // PERF_MULTI_TEST_POLICY § 1.3.1: the receive window is bounded purely by
        // an application clock (steady_clock deadline) plus a -1 (signal-driven)
        // poll wait; no poller timer object is used. The client's wire-level stop
        // token ends the phase by waking the -1 wait so the deadline can be
        // re-checked. Matches the C reference run_receive_window
        // (bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp:208-301)
        // and its is_stop_token_message handling (lines 113-116).
        while (std::chrono::steady_clock::now () < deadline) {
            const size_t ready_count =
              poller.wait (events.data (), events.size (), std::chrono::milliseconds (-1));
            if (ready_count == 0)
                continue;
            if (!(static_cast<short> (events[0].revents)
                  & static_cast<short> (zlink::poll_event_flag_t::pollin))) {
                continue;
            }

            for (;;) {
                part.init ();
                const int rc = server.recv (part, zlink::recv_flags_t::dontwait);
                if (rc != 0) {
                    if (rc == static_cast<int> (zlink::recv_result_t::no_data) || errno == EAGAIN
                        || errno == EWOULDBLOCK) {
                        part.close ();
                        break;
                    }
                    if (errno == EINTR) {
                        part.close ();
                        continue;
                    }
                    part.close ();
                    failed = true;
                    break;
                }

                // Check the wire-level stop token before decoding the payload
                // header (matches C reference lines 113-116). The stop token
                // marks active-phase end; the steady_clock deadline still bounds
                // the window.
                if (perf::multi::is_stop_token (part.data (), part.size ())) {
                    part.close ();
                    ++stop_count;
                    if (stop_count >= settings.clients)
                        break;
                    if (std::chrono::steady_clock::now () >= deadline)
                        break;
                    continue;
                }

                perf_metric::header_t header;
                if (!perf_metric::decode_payload_header (part.data (), part.size (), &header)) {
                    part.close ();
                    continue;
                }
                if (!perf_metric::is_expected (header, 1U, perf_metric::phase_active, msg_size)) {
                    part.close ();
                    continue;
                }

                ++active_count;
                latency.add (
                  perf_metric::elapsed_latency_ns (perf_metric::now_ns (), header.sent_ts_ns));
                part.close ();
            }
            if (failed)
                break;
            if (stop_count >= settings.clients)
                break;
            if (std::chrono::steady_clock::now () >= deadline)
                break;
        }

        if (failed || active_count == 0 || latency.count () == 0)
            return false;

        const perf::multi::bench_latency_stats_t latency_stats = latency.snapshot ();
        const double throughput =
          static_cast<double> (active_count) / static_cast<double> (std::max (1, active_seconds));
        const double bandwidth = throughput * static_cast<double> (msg_size) / 1000000.0;
        perf::multi::print_result (lib_name, "MULTI_DEALER_DEALER", transport, msg_size, throughput,
                                   bandwidth, latency_stats.mean_ns, latency_stats.p95_ns,
                                   latency_stats.p99_ns);
        return true;
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
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

    return perf_dealer_dealer_server (lib_name, transport, size) ? 0 : 1;
}
