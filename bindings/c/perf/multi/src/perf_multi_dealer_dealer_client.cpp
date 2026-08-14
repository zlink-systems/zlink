#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_DEALER_DEALER";
static const int k_client_socket_type = ZLINK_SOCKET_DEALER;

static std::atomic<bool> g_stop_requested (false);

enum send_status_t
{
    send_status_ok = 0,
    send_status_blocked = 1,
    send_status_fatal = 2
};

using perf_multi_client::close_client_monitors;
using perf_multi_client::close_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::refresh_connected_client_auto_hwm;
using perf_multi_client::resolve_case_msg_sizes;
using perf_multi_client::wait_client_connect_ready_all;

inline void on_signal (int)
{
    g_stop_requested.store (true, std::memory_order_release);
}

inline void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

inline bool create_client_sockets (ctx_guard_t &ctx,
                                   const std::string &transport,
                                   const std::string &endpoint,
                                   const multi_bench_settings_t &settings,
                                   size_t msg_size,
                                   std::vector<void *> *sockets_out,
                                   std::vector<ready_monitor_t> *monitors_out)
{
    return perf_multi_client::create_client_sockets (ctx, transport, endpoint, settings,
                                                     k_client_socket_type, msg_size, sockets_out,
                                                     monitors_out, false);
}

inline send_status_t send_one_message (void *socket,
                                       size_t payload_size,
                                       uint32_t run_id,
                                       perf_multi_metric::phase_t phase,
                                       size_t msg_size,
                                       uint64_t seq)
{
    if (!socket || payload_size == 0)
        return send_status_fatal;

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_size) != 0)
        return send_status_fatal;

    if (!perf_multi_metric::stamp_payload (static_cast<char *> (zlink_msg_data (&part)),
                                           payload_size, run_id, phase, msg_size, seq,
                                           perf_multi_metric::now_ns ())) {
        zlink_msg_close (&part);
        return send_status_fatal;
    }

    const zlink_submit_result_t rc = ::perf_zlink_send_parts (socket, &part, 1, ZLINK_DONTWAIT);
    if (rc == ZLINK_SUBMIT_OK)
        return send_status_ok;

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    if (err == EAGAIN)
        return send_status_blocked;

    return send_status_fatal;
}

inline bool send_stop_token (void *socket)
{
    if (!socket)
        return false;

    const size_t token_size = std::strlen (k_stop_token);
    while (!g_stop_requested.load (std::memory_order_acquire)) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, token_size) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, token_size);

        const zlink_submit_result_t rc =
          ::perf_zlink_send_parts (socket, &part, 1, ZLINK_SEND_FLAGS_NONE);
        if (rc == ZLINK_SUBMIT_OK)
            return true;

        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
            continue;
        return false;
    }

    return true;
}

inline bool run_send_window (const std::vector<void *> &sockets,
                             size_t payload_size,
                             uint32_t run_id,
                             perf_multi_metric::phase_t phase,
                             size_t msg_size,
                             double duration_seconds,
                             bool send_active,
                             uint64_t *seq)
{
    if (duration_seconds <= 0.0)
        return true;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (duration_seconds));

    if (!send_active) {
        while (!g_stop_requested.load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < deadline) {
            const long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                        deadline - std::chrono::steady_clock::now ())
                                        .count ();
            const long wait_ms = remaining_ms > 0 ? remaining_ms : 0;
            if (perf_socket_poll (NULL, 0, wait_ms) < 0 && zlink_errno () != EINTR) {
                return false;
            }
        }
        return true;
    }

    if (sockets.empty () || !seq)
        return false;

    std::vector<uint8_t> send_pending (sockets.size (), 0);
    std::vector<zlink_pollitem_t> poll_items (sockets.size ());

    if (bench_transition_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-client] send window begin phase="
                  << static_cast<unsigned int> (phase) << " msg_size=" << msg_size << std::endl;
    }

    while (!g_stop_requested.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        bool progressed = false;
        size_t pending_count = 0;
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (send_pending[i] != 0) {
                ++pending_count;
                continue;
            }

            while (!g_stop_requested.load (std::memory_order_acquire)
                   && std::chrono::steady_clock::now () < deadline) {
                const send_status_t send_rc =
                  send_one_message (sockets[i], payload_size, run_id, phase, msg_size, (*seq)++);
                if (send_rc == send_status_ok) {
                    progressed = true;
                    continue;
                }
                if (send_rc == send_status_blocked) {
                    send_pending[i] = 1;
                    ++pending_count;
                    break;
                }
                return false;
            }
        }

        if (std::chrono::steady_clock::now () >= deadline)
            continue;
        if (pending_count == 0)
            continue;

        size_t poll_count = 0;
        for (size_t i = 0; i < sockets.size (); ++i) {
            if (send_pending[i] == 0)
                continue;
            poll_items[poll_count].socket = sockets[i];
            poll_items[poll_count].fd = 0;
            poll_items[poll_count].events = ZLINK_POLLOUT;
            poll_items[poll_count].revents = 0;
            ++poll_count;
        }

        const int poll_rc = perf_socket_poll (poll_count > 0 ? &poll_items[0] : NULL,
                                              static_cast<int> (poll_count), -1);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (poll_rc == 0)
            continue;

        for (size_t i = 0; i < poll_count; ++i) {
            if ((poll_items[i].revents & ZLINK_POLLOUT) == 0)
                continue;
            for (size_t si = 0; si < sockets.size (); ++si) {
                if (sockets[si] == poll_items[i].socket) {
                    send_pending[si] = 0;
                    break;
                }
            }
        }
    }

    if (bench_transition_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-client] send window end phase="
                  << static_cast<unsigned int> (phase) << " msg_size=" << msg_size << std::endl;
    }
    return true;
}

inline bool run_single_size_case (const std::vector<void *> &sockets,
                                  const multi_bench_settings_t &settings,
                                  size_t msg_size,
                                  uint32_t run_id)
{
    const size_t payload_size = std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    uint64_t seq = 1;
    if (!run_send_window (sockets, payload_size, run_id, perf_multi_metric::phase_active, msg_size,
                          active_s, true, &seq)) {
        return false;
    }

    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!send_stop_token (sockets[i]))
            return false;
    }

    return true;
}

inline int run_client_benchmark (const std::string &lib_name,
                                 const std::string &transport,
                                 const std::string &endpoint,
                                 size_t fallback_size)
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

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> msg_sizes = resolve_case_msg_sizes (fallback_size);
    size_t max_msg_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        if (msg_sizes[i] > max_msg_size)
            max_msg_size = msg_sizes[i];
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    std::vector<void *> sockets;
    std::vector<ready_monitor_t> monitors;
    if (!create_client_sockets (ctx, transport, endpoint, settings, max_msg_size, &sockets,
                                &monitors)) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-dealer-dealer-client] create sockets failed" << std::endl;
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }

    if (!wait_client_connect_ready_all (monitors, settings.connect_ready_timeout_ms)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] connect ready timeout" << std::endl;
        }
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    close_client_monitors (&monitors);

    g_stop_requested.store (false, std::memory_order_release);
    install_signal_handlers ();

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        if (g_stop_requested.load (std::memory_order_acquire)) {
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }

        refresh_connected_client_auto_hwm (sockets,
                                           static_cast<zlink_socket_type_t> (k_client_socket_type),
                                           settings.hwm, transport, msg_sizes[si]);

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        std::cout << "CLIENT_READY," << msg_sizes[si] << std::endl;
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] ready size=" << msg_sizes[si] << std::endl;
        }
        if (!perf_multi_handshake::wait_for_start_from_stdin (msg_sizes[si])) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-client] start gate failed size=" << msg_sizes[si]
                          << std::endl;
            }
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }
        if (!run_single_size_case (sockets, settings, msg_sizes[si], run_id)) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-client] size case failed size=" << msg_sizes[si]
                          << std::endl;
            }
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] done size=" << msg_sizes[si] << std::endl;
        }
        std::cout << "CLIENT_DONE," << msg_sizes[si] << std::endl;
    }

    close_client_monitors (&monitors);
    close_client_sockets (&sockets);
    return 0;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t fallback_size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));

    std::string endpoint;
    if (!parse_endpoint_arg (argc, argv, &endpoint)) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return run_client_benchmark (lib_name, transport, endpoint, fallback_size);
}
