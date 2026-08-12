#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_DEALER_DEALER";
static const char *k_token = "dealer_dealer";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_DEALER;

using perf_multi_client::normalize_latency_stats;

enum recv_result_t
{
    recv_ok = 0,
    recv_none = 1,
    recv_fatal = 2,
    recv_stop = 3
};

inline bool decode_and_match_header (const zlink_msg_t *msg,
                                     size_t expected_msg_size,
                                     uint32_t expected_run_id,
                                     perf_multi_metric::phase_t expected_phase,
                                     perf_multi_metric::header_t *header_out)
{
    if (!msg || !header_out)
        return false;

    if (!perf_multi_metric::decode_payload_header (zlink_msg_data (const_cast<zlink_msg_t *> (msg)),
                                                   zlink_msg_size (const_cast<zlink_msg_t *> (msg)),
                                                   header_out)) {
        return false;
    }

    return header_out->magic == perf_multi_metric::k_magic && header_out->run_id == expected_run_id
           && header_out->phase == static_cast<uint32_t> (expected_phase)
           && header_out->msg_size == static_cast<uint32_t> (expected_msg_size);
}

inline recv_result_t receive_one_message (void *server,
                                          int flags,
                                          size_t expected_msg_size,
                                          uint32_t expected_run_id,
                                          perf_multi_metric::phase_t expected_phase,
                                          bool *detail_emitted,
                                          const std::string *transport,
                                          bool count_message,
                                          bool collect_latency,
                                          long *message_count,
                                          double *lat_sum,
                                          long *lat_count,
                                          bench_latency_sampler_t *lat_samples)
{
    if (!server)
        return recv_fatal;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return recv_fatal;
    const int rc = zlink_recv_part (server, &source_rid, &part, &has_more,
                                    static_cast<zlink_recv_flags_t> (flags));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR || err == ETIMEDOUT)
            return recv_none;
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] recv fatal err=" << err
                      << " size=" << expected_msg_size << " run=" << expected_run_id
                      << " phase=" << static_cast<unsigned int> (expected_phase) << std::endl;
        }
        return recv_fatal;
    }

    if (source_rid || has_more != ZLINK_PART_FINAL) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] unexpected recv metadata"
                      << " rid=" << (source_rid ? 1 : 0)
                      << " has_more=" << static_cast<int> (has_more)
                      << " size=" << expected_msg_size << " run=" << expected_run_id
                      << " phase=" << static_cast<unsigned int> (expected_phase) << std::endl;
        }
        zlink_msg_close (&part);
        return recv_fatal;
    }

    if (is_stop_token_message (part)) {
        zlink_msg_close (&part);
        return recv_stop;
    }

    perf_multi_metric::header_t header;
    const bool matched =
      decode_and_match_header (&part, expected_msg_size, expected_run_id, expected_phase, &header);

    if (!matched && bench_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-server] header mismatch expected_size="
                  << expected_msg_size << " expected_run=" << expected_run_id
                  << " expected_phase=" << static_cast<unsigned int> (expected_phase)
                  << " got_magic=" << header.magic << " got_run=" << header.run_id
                  << " got_phase=" << static_cast<unsigned int> (header.phase)
                  << " got_size=" << header.msg_size << std::endl;
    }
    if (matched && count_message && message_count)
        (*message_count)++;
    if (matched && detail_emitted && !*detail_emitted && transport) {
        perf_print_auto_hwm_snapshot (server, false, "server", *transport, true, expected_msg_size,
                                      k_server_socket_type);
        *detail_emitted = true;
    }

    if (matched && collect_latency && lat_sum && lat_count) {
        const uint64_t now_ns = perf_multi_metric::now_ns ();
        if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
            const double sample_ns = static_cast<double> (now_ns - header.sent_ts_ns);
            *lat_sum += sample_ns;
            (*lat_count)++;
            if (lat_samples)
                lat_samples->add (sample_ns);
        }
    }

    zlink_msg_close (&part);
    return recv_ok;
}

inline bool drain_non_blocking_messages (void *server,
                                         size_t expected_msg_size,
                                         uint32_t expected_run_id,
                                         perf_multi_metric::phase_t expected_phase,
                                         bool *detail_emitted,
                                         const std::string *transport,
                                         bool count_message,
                                         bool collect_latency,
                                         long *message_count,
                                         double *lat_sum,
                                         long *lat_count,
                                         bench_latency_sampler_t *lat_samples,
                                         size_t *stop_count)
{
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        const recv_result_t status =
          receive_one_message (server, ZLINK_DONTWAIT, expected_msg_size, expected_run_id,
                               expected_phase, detail_emitted, transport, count_message,
                               collect_latency, message_count, lat_sum, lat_count, lat_samples);
        if (status == recv_none)
            break;
        if (status == recv_fatal)
            return false;
        if (status == recv_stop) {
            if (stop_count)
                ++(*stop_count);
            continue;
        }
    }
    return true;
}

inline bool run_receive_window (void *server,
                                size_t expected_msg_size,
                                uint32_t expected_run_id,
                                perf_multi_metric::phase_t expected_phase,
                                bool *detail_emitted,
                                const std::string *transport,
                                double measure_seconds,
                                bool count_message,
                                bool collect_latency,
                                long *message_count,
                                double *lat_sum,
                                long *lat_count,
                                bench_latency_sampler_t *lat_samples,
                                size_t expected_stop_count)
{
    if (!server)
        return false;
    if (measure_seconds <= 0.0)
        return true;

    zlink_pollitem_t poll_item;
    poll_item.socket = server;
    poll_item.fd = 0;
    poll_item.events = ZLINK_POLLIN;
    poll_item.revents = 0;
    size_t stop_count = 0;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (measure_seconds));

    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now () >= deadline)
            break;

        poll_item.revents = 0;
        // A stop token may be consumed while draining an already-ready
        // socket.  It is the final wire signal for this phase.  Once seen,
        // wait only for the remainder of the active interval so the next
        // empty poll cannot leave the server blocked forever.
        long poll_timeout = -1;
        if (stop_count > 0) {
            const long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
              deadline - std::chrono::steady_clock::now ()).count ();
            poll_timeout = std::max<long> (1, remaining_ms);
        }
        const int poll_rc = perf_socket_poll (&poll_item, 1, poll_timeout);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (poll_rc == 0 || (poll_item.revents & ZLINK_POLLIN) == 0)
            continue;

        const recv_result_t status =
          receive_one_message (server, ZLINK_DONTWAIT, expected_msg_size, expected_run_id,
                               expected_phase, detail_emitted, transport, count_message,
                               collect_latency, message_count, lat_sum, lat_count, lat_samples);
        if (status == recv_none)
            continue;
        if (status == recv_fatal)
            return false;
        if (status == recv_stop) {
            ++stop_count;
        }

        if (std::chrono::steady_clock::now () >= deadline) {
            break;
        }

        if (!drain_non_blocking_messages (server, expected_msg_size, expected_run_id,
                                          expected_phase, detail_emitted, transport, count_message,
                                          collect_latency, message_count, lat_sum, lat_count,
                                          lat_samples, &stop_count)) {
            return false;
        }

        if (std::chrono::steady_clock::now () >= deadline) {
            break;
        }
    }

    (void) expected_stop_count;
    return true;
}

inline bool drain_phase_until_idle (void *server,
                                    size_t expected_msg_size,
                                    uint32_t expected_run_id,
                                    perf_multi_metric::phase_t expected_phase,
                                    double max_wait_seconds,
                                    int idle_wait_ms)
{
    if (!server)
        return false;

    if (max_wait_seconds <= 0.0)
        return true;

    if (idle_wait_ms <= 0)
        idle_wait_ms = 50;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (max_wait_seconds));
    std::chrono::steady_clock::time_point idle_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (idle_wait_ms);

    while (!perf_stop_requested ().load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        const recv_result_t status =
          receive_one_message (server, ZLINK_DONTWAIT, expected_msg_size, expected_run_id,
                               expected_phase, NULL, NULL, false, false, NULL, NULL, NULL, NULL);
        if (status == recv_fatal)
            return false;
        if (status == recv_ok) {
            idle_deadline =
              std::chrono::steady_clock::now () + std::chrono::milliseconds (idle_wait_ms);
            continue;
        }

        if (std::chrono::steady_clock::now () >= idle_deadline)
            return true;

        const long wait_ms = std::max<long> (
          1, std::min<long> (idle_wait_ms, std::chrono::duration_cast<std::chrono::milliseconds> (
                                             idle_deadline - std::chrono::steady_clock::now ())
                                             .count ()));
        if (perf_socket_poll (NULL, 0, wait_ms) < 0 && zlink_errno () != EINTR) {
            return false;
        }
    }

    return std::chrono::steady_clock::now () >= idle_deadline;
}

inline bool run_one_size_benchmark (void *server,
                                    const multi_bench_settings_t &settings,
                                    size_t msg_size,
                                    uint32_t run_id,
                                    const std::string &lib_name,
                                    const std::string &transport)
{
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;
    bool detail_emitted = false;
    const bool active_ok = run_receive_window (
      server, msg_size, run_id, perf_multi_metric::phase_active, &detail_emitted, &transport,
      active_s, true, true, &recv_count, &lat_sum, &lat_count, &lat_samples, settings.clients);
    if (!active_ok) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] active failed size=" << msg_size
                      << " run=" << run_id << std::endl;
        }
        return false;
    }

    // This server process is reused across size cases. Drain the tail of the
    // just-finished active phase so stale messages do not spill into the next
    // run and keep later sizes permanently behind old backlog.
    const double drain_wait_s =
      msg_size >= 65536 ? std::max (10.0, active_s * 2.0) : std::max (2.0, active_s);
    if (!drain_phase_until_idle (server, msg_size, run_id, perf_multi_metric::phase_active,
                                 drain_wait_s, 50)) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] drain failed size=" << msg_size
                      << " run=" << run_id << std::endl;
        }
        return false;
    }

    if (recv_count <= 0 || lat_count <= 0) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] active empty size=" << msg_size
                      << " run=" << run_id << " recv_count=" << recv_count
                      << " lat_count=" << lat_count << std::endl;
        }
        return false;
    }

    bench_latency_stats_t latency;
    normalize_latency_stats (lat_sum, lat_count, &lat_samples, &latency);

    const double throughput = static_cast<double> (recv_count)
                              / static_cast<double> (std::max (1, settings.duration_seconds));

    print_result (lib_name, k_pattern, transport, msg_size, throughput, latency.mean_ns,
                  latency.p95_ns, latency.p99_ns);

    return true;
}

inline int run_server_benchmark (const std::string &lib_name, const std::string &transport)
{
    set_perf_multi_pattern_env (k_pattern);

    if (!is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }

    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    void *server = zlink_socket (ctx.get (), k_server_socket_type);
    if (!server)
        return 1;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    apply_benchmark_socket_options (server, settings.hwm, transport, k_server_socket_type, 0,
                                    false);

    if (!setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint =
      bind_server_endpoint (server, transport, lib_name + std::string ("_") + k_token + "_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }

    perf_stop_requested ().store (false, std::memory_order_release);
    install_perf_signal_handlers ();

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);
    std::cout << "READY," << endpoint << std::endl;

    bool ok = true;
    for (size_t si = 0; si < sizes.size (); ++si) {
        if (perf_stop_requested ().load (std::memory_order_acquire)) {
            ok = false;
            break;
        }

        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] wait start size=" << sizes[si] << std::endl;
        }
        if (!apply_benchmark_context_auto_hwm_msg_unit (ctx.get (), sizes[si])) {
            ok = false;
            break;
        }
        apply_benchmark_hwm (server, settings.hwm);
        if (!perf_multi_handshake::wait_for_start_from_stdin (sizes[si])) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-server] start gate failed size=" << sizes[si]
                          << std::endl;
            }
            ok = false;
            break;
        }
        if (zlink_ctx_auto_hwm_recalculate (ctx.get ()) != ZLINK_CONFIG_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-server] ctx auto-hwm recalc failed err="
                          << zlink_errno () << std::endl;
            }
            ok = false;
            break;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] start size=" << sizes[si] << std::endl;
        }

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        if (!run_one_size_benchmark (server, settings, sizes[si], run_id, lib_name, transport)) {
            ok = false;
            break;
        }
    }

    perf_stop_requested ().store (true, std::memory_order_release);
    zlink_close (server);

    return ok ? 0 : 1;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    return run_server_benchmark (lib_name, transport);
}
