#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include "../common/perf_multi_measurement.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "PUBSUB";
static const char *k_token = "pubsub";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_PUB;
static const bool k_server_has_routing_id = false;
static const char *k_server_routing_id = "SERVER";
static const char *k_pubsub_topic = "bench";

static std::atomic<int> g_debug_pub_logs (0);
perf_multi_handshake::start_signal_state_t g_start_gate;
perf_multi_handshake::start_signal_state_t g_paced_gate;

bool wait_for_start_signal (size_t msg_size, int timeout_ms)
{
    if (!perf_multi_handshake::wait_for_start (&g_start_gate, msg_size, timeout_ms)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-server] start gate timeout size=" << msg_size << std::endl;
        }
        return false;
    }
    return true;
}

inline bool publish_once (void *server,
                          std::vector<char> &payload,
                          size_t current_msg_size,
                          uint32_t run_id,
                          perf_multi_metric::phase_t phase,
                          uint64_t *seq,
                          unsigned long long *publish_ok_count,
                          unsigned long long *publish_blocked_count,
                          unsigned long long *publish_wait_count)
{
    if (current_msg_size == 0)
        return true;
    if (!seq)
        return false;

    const size_t send_size =
      std::min (payload.size (), std::max<size_t> (static_cast<size_t> (1), current_msg_size));
    if (send_size < perf_multi_metric::header_size ())
        return false;
    if (!perf_multi_metric::stamp_payload (payload.data (), send_size, run_id, phase,
                                           current_msg_size, (*seq)++,
                                           perf_multi_metric::now_ns ())) {
        return false;
    }

    for (;;) {
        zlink_msg_t payload_part;
        if (zlink_msg_init_size (&payload_part, send_size) != 0)
            return false;
        std::memcpy (zlink_msg_data (&payload_part), payload.data (), send_size);

        const zlink_submit_result_t submit_rc =
          ::perf_zlink_publish_measurement_parts (
            server, k_pubsub_topic, &payload_part, ZLINK_DONTWAIT);
        const int err = submit_rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
        zlink_msg_close (&payload_part);
        if (submit_rc == ZLINK_SUBMIT_OK) {
            if (publish_ok_count)
                ++(*publish_ok_count);
            if (bench_debug_enabled ()
                && g_debug_pub_logs.fetch_add (1, std::memory_order_acq_rel) < 8) {
                std::cerr << "[multi-pubsub-server] publish ok phase="
                          << static_cast<unsigned int> (phase) << " size=" << current_msg_size
                          << " seq=" << (*seq - 1) << std::endl;
            }
            return true;
        }

        if (err == EAGAIN || err == EWOULDBLOCK) {
            if (publish_blocked_count)
                ++(*publish_blocked_count);
            if (bench_debug_enabled ()
                && g_debug_pub_logs.fetch_add (1, std::memory_order_acq_rel) < 8) {
                std::cerr << "[multi-pubsub-server] publish blocked err=" << err
                          << " phase=" << static_cast<unsigned int> (phase)
                          << " size=" << current_msg_size << std::endl;
            }
            zlink_pollitem_t item = {server, 0, ZLINK_POLLOUT, 0};
            while (!perf_stop_requested ().load (std::memory_order_acquire)) {
                item.revents = 0;
                const int poll_rc = perf_socket_poll (&item, 1, perf_aux_poll_wait_ms ());
                if (poll_rc < 0) {
                    if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                        continue;
                    return false;
                }
                if (poll_rc > 0 && (item.revents & ZLINK_POLLOUT) != 0) {
                    if (publish_wait_count)
                        ++(*publish_wait_count);
                    break;
                }
            }
            if (perf_stop_requested ().load (std::memory_order_acquire))
                return true;
            continue;
        }

        return perf_stop_requested ().load (std::memory_order_acquire);
    }
}

inline bool publish_stop_token (void *server,
                                size_t msg_size,
                                uint32_t run_id,
                                perf_multi_metric::phase_t phase)
{
    if (!server)
        return false;

    return send_stop_token_bounded (server, [msg_size, run_id, phase] (void *socket) {
        const size_t token_size = perf_multi_metric::header_size ();
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, token_size) != 0)
            return perf_stop_submit_fatal;
        if (!perf_multi_metric::stamp_payload (zlink_msg_data (&part), token_size, run_id, phase,
                                               msg_size, 0, 0)) {
            zlink_msg_close (&part);
            return perf_stop_submit_fatal;
        }

        const zlink_submit_result_t rc = ::perf_zlink_publish_measurement_parts (
          socket, k_pubsub_topic, &part, ZLINK_SEND_FLAGS_NONE);
        const int err = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
        zlink_msg_close (&part);
        if (rc == ZLINK_SUBMIT_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-server] publish stop token" << std::endl;
            }
            return perf_stop_submit_ok;
        }

        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
            return perf_stop_submit_retry;
        return perf_stop_submit_fatal;
    });
}

struct stdin_watcher_state_t
{
    stdin_watcher_state_t () : done (false) {}

    bool done;
    std::mutex mutex;
    std::condition_variable cv;
};

inline void signal_stdin_watcher_done (
  const std::shared_ptr<stdin_watcher_state_t> &state)
{
    {
        std::lock_guard<std::mutex> lock (state->mutex);
        state->done = true;
    }
    state->cv.notify_all ();
}

inline size_t resolve_max_size (const std::vector<size_t> &sizes)
{
    size_t max_size = 64;
    for (size_t i = 0; i < sizes.size (); ++i) {
        if (sizes[i] > max_size)
            max_size = sizes[i];
    }
    return max_size;
}

inline void print_server_metrics (const std::string &lib_name,
                                  const std::string &transport,
                                  const std::vector<size_t> &sizes)
{
    (void) lib_name;
    (void) transport;
    (void) sizes;
}

inline bool run_server_loop (void *ctx,
                             void *server,
                             const multi_bench_settings_t &settings,
                             const std::vector<size_t> &msg_sizes,
                             std::vector<char> *payload,
                             const std::string &lib_name,
                             const std::string &transport)
{
    if (!server || !payload)
        return false;

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t size = msg_sizes[si];
        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        if (!wait_for_start_signal (size, settings.connect_ready_timeout_ms))
            return false;
        apply_benchmark_hwm (server, settings.hwm);
        if (zlink_ctx_auto_hwm_recalculate (ctx) != ZLINK_CONFIG_OK)
            return false;
        perf_print_auto_hwm_snapshot (server, false, "server", transport, true, size,
                                      k_server_socket_type);
        uint64_t seq = 1;
        unsigned long long accepted = 0, blocked = 0, waits = 0;
        uint64_t active_sends = 0;
        const double active_s = std::max (1, settings.duration_seconds);
        const uint64_t active_end =
          perf_multi_metric::now_ns () + static_cast<uint64_t> (active_s * 1000000000.0);
        while (!perf_stop_requested ().load (std::memory_order_acquire)
               && perf_multi_metric::now_ns () < active_end) {
            if (!publish_once (server, *payload, size, run_id, perf_multi_metric::phase_active,
                               &seq, &accepted, &blocked, &waits))
                return false;
            if (perf_multi_metric::now_ns () < active_end)
                active_sends = accepted;
        }
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return false;
        if (!publish_stop_token (server, size, run_id, perf_multi_metric::phase_active))
            return false;
        std::cout << "PHASE_DONE," << size << ',' << perf_multi_metric::phase_active << std::endl;
        if (!perf_multi_handshake::wait_for_start (&g_paced_gate, size,
                                                   settings.connect_ready_timeout_ms))
            return false;
        const uint64_t epoch = perf_multi_metric::now_ns ();
        perf_multi_measurement::schedule_t schedule (
          perf_multi_measurement::target_rate (active_sends, active_s), epoch);
        if (schedule.rate <= 0.0)
            return false;
        const uint64_t end_ns =
          epoch + perf_multi_measurement::settle_ns
          + static_cast<uint64_t> (settings.latency_duration_seconds) * 1000000000ULL;
        while (!perf_stop_requested ().load (std::memory_order_acquire)
               && perf_multi_metric::now_ns () < end_ns) {
            const uint64_t now_ns = perf_multi_metric::now_ns ();
            if (now_ns >= end_ns)
                break;
            if (!schedule.due (now_ns)) {
                const uint64_t wait_ns = std::min (end_ns, schedule.next_ns ()) - now_ns;
                const long wait_ms =
                  static_cast<long> (std::min<uint64_t> (50, wait_ns / 1000000ULL));
                if (wait_ms > 0 && perf_socket_poll (NULL, 0, wait_ms) < 0
                    && zlink_errno () != EINTR)
                    return false;
                continue;
            }
            const perf_multi_metric::phase_t phase =
              perf_multi_measurement::latency_sample (now_ns, epoch,
                                                      settings.latency_duration_seconds)
                ? perf_multi_metric::phase_latency
                : perf_multi_metric::phase_warmup;
            if (!publish_once (server, *payload, size, run_id, phase, &seq, &accepted, &blocked,
                               &waits))
                return false;
            schedule.accepted (perf_multi_metric::now_ns ());
        }
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return false;
        if (!publish_stop_token (server, size, run_id, perf_multi_metric::phase_latency))
            return false;
        std::cout << "PHASE_DONE," << size << ',' << perf_multi_metric::phase_latency << std::endl;
        perf_multi_measurement::print_diagnostic (k_pattern, size, active_sends, active_s,
                                                  schedule);
    }
    (void) lib_name;
    // Production completed; the receiver's normal STOP may already be present.
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
    const int linger_ms = 0;
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    const int nodrop = resolve_multi_int_env (
      "PERF_MULTI_PUBSUB_XPUB_NODROP", 0, 0);
    if (nodrop > 1
        || !set_pub_opt_int (server, ZLINK_PUB_OPT_NODROP, nodrop,
                          "ZLINK_PUB_OPT_NODROP")) {
        zlink_close (server);
        return 1;
    }
    apply_benchmark_hwm (server, settings.hwm);
    if (k_server_has_routing_id) {
        zlink_set_routing_id (server, k_server_routing_id, std::strlen (k_server_routing_id));
    }

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
    perf_multi_handshake::reset_start_signal_state (&g_start_gate);
    perf_multi_handshake::reset_start_signal_state (&g_paced_gate);
    install_perf_signal_handlers ();

    const std::shared_ptr<stdin_watcher_state_t> stdin_watcher_state (
      new stdin_watcher_state_t ());
    std::thread stdin_watcher ([stdin_watcher_state] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t start_size = 0;
            if (perf_multi_handshake::parse_size_command_line (line, "START,", &start_size)) {
                perf_multi_handshake::signal_start (&g_start_gate, start_size);
                continue;
            }
            if (perf_multi_handshake::parse_size_command_line (line, "PHASE_PACED,", &start_size)) {
                perf_multi_handshake::signal_start (&g_paced_gate, start_size);
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                perf_multi_handshake::signal_stop (&g_start_gate);
                perf_multi_handshake::signal_stop (&g_paced_gate);
                signal_stdin_watcher_done (stdin_watcher_state);
                return;
            }
        }
        if (bench_debug_enabled ())
            std::cerr << "[multi-pubsub-server] stdin watcher eof" << std::endl;
        perf_stop_requested ().store (true, std::memory_order_release);
        perf_multi_handshake::signal_stop (&g_start_gate);
        perf_multi_handshake::signal_stop (&g_paced_gate);
        signal_stdin_watcher_done (stdin_watcher_state);
    });

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);

    const size_t max_size = resolve_max_size (sizes);
    std::vector<char> payload (
      std::max<size_t> (static_cast<size_t> (1024),
                        std::max<size_t> (max_size, perf_multi_metric::header_size ())),
      's');
    std::cout << "READY," << endpoint << std::endl;

    const bool loop_ok =
      run_server_loop (ctx.get (), server, settings, sizes, &payload, lib_name, transport);

    bool stdin_watcher_done = false;
    if (loop_ok) {
        const int wait_ms = perf_stop_send_timeout_ms () * perf_stop_send_max_attempts ();
        std::unique_lock<std::mutex> lock (stdin_watcher_state->mutex);
        stdin_watcher_done = stdin_watcher_state->cv.wait_for (
          lock, std::chrono::milliseconds (wait_ms), [stdin_watcher_state] () {
              return stdin_watcher_state->done;
          });
    }

    perf_stop_requested ().store (true, std::memory_order_release);
    perf_multi_handshake::signal_stop (&g_start_gate);
    perf_multi_handshake::signal_stop (&g_paced_gate);
    if (stdin_watcher.joinable ()) {
        if (stdin_watcher_done)
            stdin_watcher.join ();
        else
            stdin_watcher.detach ();
    }

    print_server_metrics (lib_name, transport, sizes);

    zlink_close (server);

    return loop_ok ? 0 : 1;
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
