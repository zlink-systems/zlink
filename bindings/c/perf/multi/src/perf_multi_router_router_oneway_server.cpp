#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

const char *const k_pattern = "MULTI_ROUTER_ROUTER_ONEWAY";
const char *const k_server_routing_id = "SERVER";
perf_multi_handshake::start_signal_state_t g_start_gate;

zlink_routing_id_t peer_routing_id (size_t index)
{
    zlink_routing_id_t rid;
    std::memset (&rid, 0, sizeof (rid));
    char text[32];
    const int length = std::snprintf (text, sizeof (text), "peer-%zu", index);
    if (length > 0 && static_cast<size_t> (length) <= sizeof (rid.data)) {
        rid.size = static_cast<uint8_t> (length);
        std::memcpy (rid.data, text, static_cast<size_t> (length));
    }
    return rid;
}

bool send_to_peer (void *server,
                   const zlink_routing_id_t &target,
                   const void *data,
                   size_t size,
                   zlink_send_flags_t flags)
{
    zlink_msg_t part;
    if (zlink_msg_init_size (&part, size) != 0)
        return false;
    std::memcpy (zlink_msg_data (&part), data, size);
    const zlink_submit_result_t rc =
      perf_zlink_send_rid_parts (server, &target, &part, 1, flags);
    if (rc != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return rc == ZLINK_SUBMIT_OK;
}

bool send_active_fanout (void *server,
                         const std::vector<zlink_routing_id_t> &targets,
                         std::vector<char> *payload,
                         size_t msg_size,
                         uint32_t run_id,
                         uint64_t sequence)
{
    if (!server || !payload)
        return false;
    const size_t payload_size =
      std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    if (payload->size () < payload_size)
        return false;
    std::memset (payload->data (), 'r', payload_size);
    if (!perf_multi_metric::stamp_payload (payload->data (), payload_size, run_id,
                                           perf_multi_metric::phase_active, msg_size, sequence,
                                           perf_multi_metric::now_ns ()))
        return false;

    bool admitted = false;
    for (size_t i = 0; i < targets.size (); ++i) {
        if (send_to_peer (server, targets[i], payload->data (), payload_size,
                          ZLINK_SEND_FLAGS_DONTWAIT)) {
            admitted = true;
            continue;
        }
        const int err = zlink_errno ();
        if (err == EAGAIN || err == EINTR || err == EHOSTUNREACH || err == ENOTCONN)
            continue;
        return false;
    }
    return admitted;
}

bool send_cooldown_to_all (void *server,
                           const std::vector<zlink_routing_id_t> &targets,
                           std::vector<char> *payload,
                           size_t msg_size,
                           uint32_t run_id,
                           uint64_t *sequence)
{
    if (!server || !payload || !sequence)
        return false;

    const size_t payload_size =
      std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    if (payload->size () < payload_size)
        return false;
    std::memset (payload->data (), 'r', payload_size);
    if (!perf_multi_metric::stamp_payload (payload->data (), payload_size, run_id,
                                           perf_multi_metric::phase_cooldown, msg_size,
                                           (*sequence)++, perf_multi_metric::now_ns ()))
        return false;

    std::vector<bool> pending (targets.size (), true);
    size_t pending_count = targets.size ();
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (25);

    while (pending_count > 0 && std::chrono::steady_clock::now () < deadline) {
        bool admitted = false;
        for (size_t i = 0; i < targets.size (); ++i) {
            if (!pending[i])
                continue;
            if (!send_to_peer (server, targets[i], payload->data (), payload_size,
                               ZLINK_SEND_FLAGS_DONTWAIT)) {
                const int err = zlink_errno ();
                if (err != EAGAIN && err != EINTR && err != EHOSTUNREACH
                    && err != ENOTCONN)
                    return false;
                continue;
            }
            pending[i] = false;
            --pending_count;
            admitted = true;
        }
        if (!admitted)
            std::this_thread::yield ();
    }

    return pending_count == 0;
}

int run_server (const std::string &lib_name, const std::string &transport)
{
    set_perf_multi_pattern_env (k_pattern);
    if (!perf_multi_client::is_supported_transport (transport)
        || !transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;
    if (zlink_ctx_set (ctx.get (), ZLINK_IO_THREADS, 1) != 0)
        return 1;

    void *server = zlink_socket (ctx.get (), ZLINK_SOCKET_ROUTER);
    if (!server)
        return 1;
    const int linger = 0;
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger, "ZLINK_OPT_LINGER");
    apply_benchmark_hwm (server, settings.hwm);
    if (zlink_set_routing_id (server, k_server_routing_id,
                              std::strlen (k_server_routing_id))
          != ZLINK_CONFIG_OK
        || !setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint =
      bind_server_endpoint (server, transport, lib_name + "_router_router_oneway_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }

    std::vector<zlink_routing_id_t> targets;
    targets.reserve (settings.clients);
    for (size_t i = 0; i < settings.clients; ++i)
        targets.push_back (peer_routing_id (i));

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);
    size_t max_size = perf_multi_metric::header_size ();
    for (size_t i = 0; i < sizes.size (); ++i)
        max_size = std::max (max_size, sizes[i]);
    std::vector<char> payload (max_size, 'r');

    perf_stop_requested ().store (false, std::memory_order_release);
    perf_multi_handshake::reset_start_signal_state (&g_start_gate);
    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t size = 0;
            if (perf_multi_handshake::parse_size_command_line (line, "START,", &size)) {
                perf_multi_handshake::signal_start (&g_start_gate, size);
            } else if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                perf_multi_handshake::signal_stop (&g_start_gate);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
        perf_multi_handshake::signal_stop (&g_start_gate);
    });

    std::cout << "READY," << endpoint << std::endl;
    bool ok = true;
    for (size_t si = 0; si < sizes.size () && ok; ++si) {
        const size_t msg_size = sizes[si];
        if (!perf_multi_handshake::wait_for_start (
              &g_start_gate, msg_size, settings.connect_ready_timeout_ms)) {
            ok = false;
            break;
        }
        if (!apply_benchmark_context_auto_hwm_msg_unit (ctx.get (), msg_size)) {
            ok = false;
            break;
        }
        apply_benchmark_hwm (server, settings.hwm);
        if (zlink_ctx_auto_hwm_recalculate (ctx.get ()) != ZLINK_CONFIG_OK) {
            ok = false;
            break;
        }
        perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                      ZLINK_SOCKET_ROUTER);

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        uint64_t sequence = 1;
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::seconds (std::max (1, settings.duration_seconds));
        while (!perf_stop_requested ().load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < deadline) {
            if (!send_active_fanout (server, targets, &payload, msg_size, run_id,
                                     sequence++)) {
                std::this_thread::yield ();
            }
        }
        ok = send_cooldown_to_all (server, targets, &payload, msg_size, run_id, &sequence);
    }

    perf_stop_requested ().store (true, std::memory_order_release);
    perf_multi_handshake::signal_stop (&g_start_gate);
    if (stdin_watcher.joinable ())
        stdin_watcher.join ();
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
    return run_server (argv[1], argv[2]);
}
