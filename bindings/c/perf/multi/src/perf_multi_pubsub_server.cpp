#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
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

        if (::perf_zlink_publish_parts (server, k_pubsub_topic, &payload_part,
                                         1, ZLINK_DONTWAIT) == ZLINK_SUBMIT_OK) {
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

        const int err = zlink_errno ();
        if (err == EAGAIN || err == EWOULDBLOCK) {
            if (publish_blocked_count)
                ++(*publish_blocked_count);
            if (bench_debug_enabled ()
                && g_debug_pub_logs.fetch_add (1, std::memory_order_acq_rel) < 8) {
                std::cerr << "[multi-pubsub-server] publish blocked err=" << err
                          << " phase=" << static_cast<unsigned int> (phase)
                          << " size=" << current_msg_size << std::endl;
            }
            zlink_msg_close (&payload_part);

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

inline bool publish_stop_token (void *server)
{
    if (!server)
        return false;

    const size_t token_size = std::strlen (k_stop_token);
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, token_size) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, token_size);

        const zlink_submit_result_t rc =
          ::perf_zlink_publish_parts (server, k_pubsub_topic, &part, 1, ZLINK_SEND_FLAGS_NONE);
        if (rc == ZLINK_SUBMIT_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-server] publish stop token" << std::endl;
            }
            return true;
        }

        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
            continue;
        return false;
    }

    return true;
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

struct one_way_phase_t
{
    one_way_phase_t (size_t msg_size_,
                     perf_multi_metric::phase_t phase_,
                     std::chrono::steady_clock::duration duration_,
                     bool send_active_) :
        msg_size (msg_size_), phase (phase_), duration (duration_), send_active (send_active_)
    {
    }

    size_t msg_size;
    perf_multi_metric::phase_t phase;
    std::chrono::steady_clock::duration duration;
    bool send_active;
};

inline void append_one_way_phase (std::vector<one_way_phase_t> *phases,
                                  size_t msg_size,
                                  perf_multi_metric::phase_t phase,
                                  double seconds,
                                  bool send_active)
{
    if (!phases || seconds <= 0.0)
        return;
    phases->push_back (
      one_way_phase_t (msg_size, phase,
                       std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                         std::chrono::duration<double> (seconds)),
                       send_active));
}

inline std::vector<one_way_phase_t> build_one_way_phases (const multi_bench_settings_t &settings,
                                                          const std::vector<size_t> &msg_sizes)
{
    std::vector<one_way_phase_t> phases;
    if (msg_sizes.empty ())
        return phases;

    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        const size_t msg_size = msg_sizes[i];
        append_one_way_phase (&phases, msg_size, perf_multi_metric::phase_active, active_s, true);
    }

    return phases;
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

    const std::vector<one_way_phase_t> phases = build_one_way_phases (settings, msg_sizes);
    size_t phase_index = 0;
    auto phase_deadline = std::chrono::steady_clock::time_point ();
    bool phase_started = false;
    size_t current_phase_msg_size = 0;
    uint32_t current_phase_run_id = 0;
    perf_multi_metric::phase_t current_phase = perf_multi_metric::phase_unknown;
    uint64_t phase_seq = 1;
    unsigned long long publish_ok_count = 0;
    unsigned long long publish_blocked_count = 0;
    unsigned long long publish_wait_count = 0;

    auto log_phase_summary = [&] (const char *reason) {
        if (!bench_transition_debug_enabled ())
            return;
        std::cerr << "[multi-pubsub-server] phase summary reason=" << reason
                  << " size=" << current_phase_msg_size
                  << " phase=" << static_cast<unsigned int> (current_phase)
                  << " ok=" << publish_ok_count << " blocked=" << publish_blocked_count
                  << " wait=" << publish_wait_count
                  << " seq=" << (phase_seq > 0 ? phase_seq - 1 : 0) << std::endl;
    };

    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        if (!phases.empty ()) {
            const auto now = std::chrono::steady_clock::now ();
            while (phase_started && phase_index < phases.size () && now >= phase_deadline) {
                log_phase_summary ("deadline");
                if (current_phase == perf_multi_metric::phase_active
                    && !publish_stop_token (server)) {
                    return false;
                }
                ++phase_index;
                phase_started = false;
                publish_ok_count = 0;
                publish_blocked_count = 0;
                publish_wait_count = 0;
            }

            if (phase_index >= phases.size ()) {
                // The benchmark phases are fully scripted for PUBSUB, so once
                // active is complete there is nothing left to wait for.
                // Exiting here avoids an extra STOP-driven shutdown edge.
                return true;
            }

            if (phases[phase_index].msg_size != current_phase_msg_size
                || phases[phase_index].phase != current_phase) {
                const bool new_size = phases[phase_index].msg_size != current_phase_msg_size;
                current_phase_msg_size = phases[phase_index].msg_size;
                current_phase_run_id = static_cast<uint32_t> (phase_index + 1);
                current_phase = phases[phase_index].phase;
                phase_seq = 1;
                publish_ok_count = 0;
                publish_blocked_count = 0;
                publish_wait_count = 0;
                if (new_size
                    && !wait_for_start_signal (current_phase_msg_size,
                                               settings.connect_ready_timeout_ms)) {
                    return false;
                }
                if (new_size) {
                    if (!apply_benchmark_context_auto_hwm_msg_unit (ctx, current_phase_msg_size))
                        return false;
                    apply_benchmark_hwm (server, settings.hwm);
                    if (zlink_ctx_auto_hwm_recalculate (ctx) != ZLINK_CONFIG_OK) {
                        if (bench_debug_enabled ()) {
                            std::cerr << "[multi-pubsub-server] ctx auto-hwm recalc failed err="
                                      << zlink_errno () << std::endl;
                        }
                        return false;
                    }
                    perf_print_auto_hwm_snapshot (server, false, "server", transport, true,
                                                  current_phase_msg_size, k_server_socket_type);
                }
                phase_deadline = std::chrono::steady_clock::now () + phases[phase_index].duration;
                phase_started = true;
            }

            if (!phases[phase_index].send_active) {
                if (!phase_started) {
                    phase_deadline =
                      std::chrono::steady_clock::now () + phases[phase_index].duration;
                    phase_started = true;
                }
                const long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                            phase_deadline - std::chrono::steady_clock::now ())
                                            .count ();
                const long wait_ms = remaining_ms > 0 ? remaining_ms : 0;
                if (perf_socket_poll (NULL, 0, wait_ms) < 0 && zlink_errno () != EINTR) {
                    return false;
                }
                continue;
            }

            if (!publish_once (server, *payload, phases[phase_index].msg_size, current_phase_run_id,
                               phases[phase_index].phase, &phase_seq, &publish_ok_count,
                               &publish_blocked_count, &publish_wait_count)) {
                return false;
            }
            continue;
        }

        current_phase = perf_multi_metric::phase_active;
        current_phase_msg_size = payload->size ();
        current_phase_run_id = 1;
        if (!publish_once (server, *payload, payload->size (), current_phase_run_id,
                           perf_multi_metric::phase_active, &phase_seq, &publish_ok_count,
                           &publish_blocked_count, &publish_wait_count)) {
            return false;
        }
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
    install_perf_signal_handlers ();

    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t start_size = 0;
            if (perf_multi_handshake::parse_size_command_line (line, "START,", &start_size)) {
                perf_multi_handshake::signal_start (&g_start_gate, start_size);
                continue;
            }
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                perf_multi_handshake::signal_stop (&g_start_gate);
                return;
            }
        }
        if (bench_debug_enabled ())
            std::cerr << "[multi-pubsub-server] stdin watcher eof" << std::endl;
        perf_stop_requested ().store (true, std::memory_order_release);
        perf_multi_handshake::signal_stop (&g_start_gate);
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

    perf_stop_requested ().store (true, std::memory_order_release);
    perf_multi_handshake::signal_stop (&g_start_gate);
    if (stdin_watcher.joinable ()) {
        stdin_watcher.join ();
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
