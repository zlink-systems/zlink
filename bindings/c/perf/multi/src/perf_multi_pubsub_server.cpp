#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_PUBSUB";
static const char *k_token = "pubsub";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_PUB;
static const bool k_server_has_routing_id = false;
static const char *k_server_routing_id = "SERVER";
static const char *k_pubsub_topic = "bench";

static std::atomic<int> g_debug_pub_logs (0);
perf_multi_handshake::start_signal_state_t g_start_gate;

struct latency_control_state_t
{
    latency_control_state_t () : ready_sizes (), acked_sequences (), stopped (false) {}

    std::set<size_t> ready_sizes;
    std::map<size_t, uint64_t> acked_sequences;
    bool stopped;
    std::mutex mutex;
    std::condition_variable cv;
};

latency_control_state_t g_latency_control;

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

inline void reset_latency_control ()
{
    std::lock_guard<std::mutex> lock (g_latency_control.mutex);
    g_latency_control.ready_sizes.clear ();
    g_latency_control.acked_sequences.clear ();
    g_latency_control.stopped = false;
}

inline void signal_latency_ready (size_t msg_size)
{
    {
        std::lock_guard<std::mutex> lock (g_latency_control.mutex);
        g_latency_control.ready_sizes.insert (msg_size);
    }
    g_latency_control.cv.notify_all ();
}

inline void signal_latency_ack (size_t msg_size, uint64_t seq)
{
    {
        std::lock_guard<std::mutex> lock (g_latency_control.mutex);
        uint64_t &acked = g_latency_control.acked_sequences[msg_size];
        if (seq > acked)
            acked = seq;
    }
    g_latency_control.cv.notify_all ();
}

inline void signal_latency_stop ()
{
    {
        std::lock_guard<std::mutex> lock (g_latency_control.mutex);
        g_latency_control.stopped = true;
    }
    g_latency_control.cv.notify_all ();
}

inline bool wait_for_latency_ready (size_t msg_size, int timeout_ms)
{
    std::unique_lock<std::mutex> lock (g_latency_control.mutex);
    const bool signaled = g_latency_control.cv.wait_for (
      lock, std::chrono::milliseconds (std::max (1, timeout_ms)), [msg_size] () {
          return g_latency_control.stopped
                 || g_latency_control.ready_sizes.find (msg_size)
                      != g_latency_control.ready_sizes.end ();
      });
    if (!signaled || g_latency_control.stopped) {
        errno = g_latency_control.stopped ? ECANCELED : ETIMEDOUT;
        return false;
    }
    g_latency_control.ready_sizes.erase (msg_size);
    return true;
}

inline bool wait_for_latency_ack (size_t msg_size, uint64_t seq, int timeout_ms)
{
    std::unique_lock<std::mutex> lock (g_latency_control.mutex);
    const bool signaled = g_latency_control.cv.wait_for (
      lock, std::chrono::milliseconds (std::max (1, timeout_ms)), [msg_size, seq] () {
          const std::map<size_t, uint64_t>::const_iterator it =
            g_latency_control.acked_sequences.find (msg_size);
          return g_latency_control.stopped
                 || (it != g_latency_control.acked_sequences.end () && it->second >= seq);
      });
    if (!signaled || g_latency_control.stopped) {
        errno = g_latency_control.stopped ? ECANCELED : ETIMEDOUT;
        return false;
    }
    return true;
}

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
                          unsigned long long *publish_wait_count,
                          const std::chrono::steady_clock::time_point *deadline,
                          bool *published_out)
{
    if (published_out)
        *published_out = false;
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
            if (published_out)
                *published_out = true;
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
                if (deadline && std::chrono::steady_clock::now () >= *deadline)
                    return true;
                item.revents = 0;
                int wait_ms = perf_aux_poll_wait_ms ();
                if (deadline) {
                    const long long remaining_ms =
                      std::chrono::duration_cast<std::chrono::milliseconds> (
                        *deadline - std::chrono::steady_clock::now ())
                        .count ();
                    wait_ms = static_cast<int> (
                      std::max<long long> (1, std::min<long long> (wait_ms, remaining_ms)));
                }
                const int poll_rc = perf_socket_poll (&item, 1, wait_ms);
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

inline bool publish_stop_token (void *server)
{
    if (!server)
        return false;

    return send_stop_token_bounded (server, [] (void *socket) {
        const size_t token_size = std::strlen (k_stop_token);
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, token_size) != 0)
            return perf_stop_submit_fatal;
        std::memcpy (zlink_msg_data (&part), k_stop_token, token_size);

        const zlink_submit_result_t rc =
          ::perf_zlink_publish_parts (socket, k_pubsub_topic, &part, 1, ZLINK_SEND_FLAGS_NONE);
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

inline bool publish_active_stop_barrier (void *server, size_t msg_size, int timeout_ms)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (1, timeout_ms));
    for (;;) {
        // Re-publish at a bounded cadence until the client confirms that
        // every SUB observed the ordered NODROP barrier.
        if (!publish_stop_token (server))
            return false;
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        const int remaining_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ());
        if (wait_for_latency_ready (msg_size, std::min (20, std::max (1, remaining_ms))))
            return true;
        if (errno != ETIMEDOUT)
            return false;
    }
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

    const int control_timeout_ms = std::max (1, settings.connect_ready_timeout_ms);
    for (size_t size_index = 0; size_index < msg_sizes.size (); ++size_index) {
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return false;

        const size_t msg_size = msg_sizes[size_index];
        const uint32_t run_id = static_cast<uint32_t> (size_index + 1);
        if (!wait_for_start_signal (msg_size, settings.connect_ready_timeout_ms))
            return false;

        apply_benchmark_hwm (server, settings.hwm);
        if (zlink_ctx_auto_hwm_recalculate (ctx) != ZLINK_CONFIG_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-server] ctx auto-hwm recalc failed err="
                          << zlink_errno () << std::endl;
            }
            return false;
        }
        perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                      k_server_socket_type);

        uint64_t active_seq = 1;
        unsigned long long active_ok = 0;
        unsigned long long active_blocked = 0;
        unsigned long long active_wait = 0;
        const std::chrono::steady_clock::time_point active_deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::seconds (std::max (1, settings.duration_seconds));
        while (std::chrono::steady_clock::now () < active_deadline
               && !perf_stop_requested ().load (std::memory_order_acquire)) {
            bool published = false;
            if (!publish_once (server, *payload, msg_size, run_id,
                               perf_multi_metric::phase_active, &active_seq, &active_ok,
                               &active_blocked, &active_wait, &active_deadline, &published)) {
                return false;
            }
            if (!published && std::chrono::steady_clock::now () >= active_deadline)
                break;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-pubsub-server] active summary size=" << msg_size
                      << " ok=" << active_ok << " blocked=" << active_blocked
                      << " wait=" << active_wait << std::endl;
        }

        // The marker follows every admitted active record on each subscriber
        // pipe.  The client does not begin latency traffic until every SUB has
        // consumed it, which also drains the active backlog.
        if (!publish_active_stop_barrier (server, msg_size, control_timeout_ms))
            return false;

        uint64_t latency_seq = 1;
        unsigned long long latency_ok = 0;
        unsigned long long latency_blocked = 0;
        unsigned long long latency_wait = 0;
        const std::chrono::steady_clock::time_point latency_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (std::chrono::steady_clock::now () < latency_deadline
               && !perf_stop_requested ().load (std::memory_order_acquire)) {
            bool published = false;
            if (!publish_once (server, *payload, msg_size, run_id,
                               perf_multi_metric::phase_latency, &latency_seq, &latency_ok,
                               &latency_blocked, &latency_wait, &latency_deadline, &published)) {
                return false;
            }
            if (!published)
                break;
            const uint64_t sent_seq = latency_seq - 1;
            bool acked = false;
            while (std::chrono::steady_clock::now () < latency_deadline) {
                const std::chrono::steady_clock::time_point ack_now =
                  std::chrono::steady_clock::now ();
                const long long remaining_ms =
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    latency_deadline - ack_now)
                    .count ();
                const int ack_timeout_ms = static_cast<int> (
                  std::max<long long> (1,
                                       std::min<long long> (control_timeout_ms,
                                                            remaining_ms)));
                if (wait_for_latency_ack (msg_size, sent_seq, ack_timeout_ms)) {
                    acked = true;
                    break;
                }
                if (errno != ETIMEDOUT)
                    return false;
            }
            if (!acked)
                break;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-pubsub-server] latency summary size=" << msg_size
                      << " ok=" << latency_ok << " blocked=" << latency_blocked
                      << " wait=" << latency_wait << std::endl;
        }

        if (!publish_stop_token (server))
            return false;
    }

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
    if (!set_pub_opt_int (server, ZLINK_PUB_OPT_NODROP, 1,
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
    reset_latency_control ();
    install_perf_signal_handlers ();

    const std::shared_ptr<stdin_watcher_state_t> stdin_watcher_state (
      new stdin_watcher_state_t ());
    std::thread stdin_watcher ([stdin_watcher_state] () {
        std::string line;
        bool explicit_stop = false;
        while (std::getline (std::cin, line)) {
            size_t start_size = 0;
            if (perf_multi_handshake::parse_size_command_line (line, "START,", &start_size)) {
                perf_multi_handshake::signal_start (&g_start_gate, start_size);
                continue;
            }
            size_t latency_size = 0;
            if (perf_multi_handshake::parse_size_command_line (
                  line, "LATENCY_READY,", &latency_size)) {
                signal_latency_ready (latency_size);
                continue;
            }
            size_t latency_seq = 0;
            if (perf_multi_handshake::parse_size_count_command_line (
                  line, "LATENCY_ACK,", &latency_size, &latency_seq)) {
                signal_latency_ack (latency_size, static_cast<uint64_t> (latency_seq));
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                explicit_stop = true;
                break;
            }
        }
        if (!explicit_stop && bench_debug_enabled ())
            std::cerr << "[multi-pubsub-server] stdin watcher eof" << std::endl;
        perf_stop_requested ().store (true, std::memory_order_release);
        perf_multi_handshake::signal_stop (&g_start_gate);
        signal_latency_stop ();
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
        // A successful final publish is only local admission. Keep the PUB
        // socket alive until the client reports CLIENT_DONE through the
        // runner's STOP/EOF handshake, but never turn that acknowledgement
        // into an unbounded server shutdown wait.
        std::unique_lock<std::mutex> lock (stdin_watcher_state->mutex);
        stdin_watcher_done = stdin_watcher_state->cv.wait_for (
          lock, std::chrono::seconds (10), [stdin_watcher_state] () {
              return stdin_watcher_state->done;
          });
    }

    perf_stop_requested ().store (true, std::memory_order_release);
    perf_multi_handshake::signal_stop (&g_start_gate);
    signal_latency_stop ();

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
