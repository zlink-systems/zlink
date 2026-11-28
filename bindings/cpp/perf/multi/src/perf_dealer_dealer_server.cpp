// DEALER-DEALER multi server benchmark: one-way receive sink.
// Topology: client DEALER(connect, N) -> server DEALER(bind, 1)
// Measurement role: drain incoming payloads and emit server queue metrics.

#include "../common/perf_common.hpp"
#include "../common/perf_dealer_dealer_receive.hpp"
#include "../common/perf_multi_measurement.hpp"
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
        std::cout << "UNSUPPORTED," << lib_name << ",DEALER_DEALER," << transport
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
        if (!perf::multi::recalculate_auto_hwm (ctx))
            return false;

        using clock_t = std::chrono::steady_clock;
        using perf_multi_measurement::settle_duration;
        const int active_seconds = std::max (1, settings.duration_seconds);
        const size_t payload_size = std::max<size_t> (msg_size, perf_metric::header_size ());
        bool auto_hwm_detail_emitted = false;
        perf::multi::bench_latency_sampler_t latency;
        zlink::poll_event_t event;
        const auto receive_phase = [&] (perf_metric::phase_t phase_, clock_t::duration duration_,
                                        uint64_t &count_) {
            const bool paced = phase_ == perf_metric::phase_latency;
            const auto start = clock_t::now ();
            const auto deadline = start + duration_;
            const auto sample_start = paced ? start + settle_duration : start;
            const double measure_seconds = std::chrono::duration<double> (duration_).count ();
            const double drain_seconds = msg_size >= 65536 ? std::max (10.0, measure_seconds * 2.0)
                                                           : std::max (2.0, measure_seconds);
            const auto drain_deadline = deadline
                                        + std::chrono::duration_cast<clock_t::duration> (
                                          std::chrono::duration<double> (drain_seconds));
            size_t stop_count = 0;
            for (;;) {
                const auto now = clock_t::now ();
                if (now >= drain_deadline)
                    return false;
                zlink::received_t received;
                const int rc = server.recv (received, zlink::recv_flags_t::dontwait);
                if (rc != 0) {
                    if (errno == EINTR)
                        continue;
                    if (rc != static_cast<int> (zlink::recv_result_t::no_data) && errno != EAGAIN
                        && errno != EWOULDBLOCK)
                        return false;
                    const auto remaining = perf::multi::remaining_bounded_timeout (drain_deadline);
                    (void) poller.wait (&event, 1,
                                        std::min (remaining, std::chrono::milliseconds (10)));
                    continue;
                }

                const auto &parts = received.parts ();
                if (parts.size () == 1
                    && perf::multi::is_stop_token (parts.front ().data (), parts.front ().size ())) {
                    if (++stop_count == settings.clients)
                        return true;
                    continue;
                }
                if (!perf::multi::measurement_parts_valid (parts))
                    continue;
                const auto &payload = parts.front ();
                perf_metric::header_t header;
                if (payload.size () != payload_size
                    || !perf_metric::decode_payload_header (payload.data (), payload.size (),
                                                            &header))
                    continue;
                if (!perf::multi::record_dealer_dealer_receive (
                      header, phase_, msg_size, now, sample_start, deadline, count_))
                    continue;
                if (paced) {
                    latency.add (
                      perf_metric::elapsed_latency_ns (perf_metric::now_ns (), header.sent_ts_ns));
                } else if (!auto_hwm_detail_emitted) {
                    perf::multi::emit_auto_hwm_detail (server, "server", "server", transport,
                                                       msg_size, "dealer");
                    auto_hwm_detail_emitted = true;
                }
            }
        };

        uint64_t active_count = 0;
        if (!receive_phase (perf_metric::phase_active, std::chrono::seconds (active_seconds),
                            active_count)
            || active_count == 0)
            return false;
        std::cout << "PHASE_DRAINED," << msg_size << ',' << active_count << std::endl;

        uint64_t paced_received = 0;
        if (!receive_phase (perf_metric::phase_latency,
                            settle_duration
                              + std::chrono::seconds (settings.latency_duration_seconds),
                            paced_received)
            || latency.count () == 0)
            return false;
        const perf::multi::bench_latency_stats_t latency_stats = latency.snapshot ();
        const double throughput =
          static_cast<double> (active_count) / static_cast<double> (std::max (1, active_seconds));
        const double bandwidth = throughput * static_cast<double> (msg_size) / 1000000.0;
        perf::multi::print_result (lib_name, "DEALER_DEALER", transport, msg_size, throughput,
                                   bandwidth, latency_stats.mean_ns, latency_stats.p95_ns,
                                   latency_stats.p99_ns);
        std::cout << "PHASE_COMPLETE," << msg_size << "," << paced_received << std::endl;
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
