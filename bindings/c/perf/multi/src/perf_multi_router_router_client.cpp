#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_ROUTER_ROUTER_SENDSEND";
static const zlink_socket_type_t k_client_socket_type = ZLINK_SOCKET_ROUTER;
static const bool k_client_router_send = true;
static const char *k_server_routing_id = "SERVER";

using perf_multi_client::close_client_monitors;
using perf_multi_client::close_client_sockets;
using perf_multi_client::create_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::next_metric_run_id;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::print_echo_client_result_lines;
using perf_multi_client::refresh_connected_client_auto_hwm;
using perf_multi_client::resolve_case_max_msg_size;
using perf_multi_client::resolve_case_msg_sizes;
using perf_multi_client::run_echo_duration;
using perf_multi_client::wait_client_connect_ready_all;

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
    const size_t max_msg_size = resolve_case_max_msg_size (fallback_size, msg_sizes);
    const bool per_socket_payload = transport == "tcp";

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    const std::string server_id = k_server_routing_id ? k_server_routing_id : "SERVER";
    const size_t payload_capacity =
      std::max<size_t> (max_msg_size, perf_multi_metric::header_size ());
    const size_t scratch_capacity = perf_multi_client::metric_capture_bytes ();
    std::vector<char> payload (payload_capacity, 'c');

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];
        const size_t payload_size = std::max<size_t> (msg_size, perf_multi_metric::header_size ());
        const uint32_t run_id = next_metric_run_id ();
        std::vector<void *> sockets;
        std::vector<ready_monitor_t> monitors;
        if (!create_client_sockets (ctx, transport, endpoint, settings, k_client_socket_type,
                                    msg_size, &sockets, &monitors, false)) {
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }

        if (!wait_client_connect_ready_all (monitors, settings.connect_ready_timeout_ms)) {
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }
        close_client_monitors (&monitors);
        refresh_connected_client_auto_hwm (sockets, k_client_socket_type, settings.hwm,
                                           transport, msg_size);

        double throughput = 0.0;
        bench_latency_stats_t latency;
        if (!run_echo_duration (sockets, settings, payload, payload_size, msg_size,
                                scratch_capacity, server_id, k_client_router_send,
                                per_socket_payload, run_id, &throughput, &latency)) {
            close_client_sockets (&sockets);
            return 1;
        }

        print_echo_client_result_lines (k_pattern, lib_name, transport, msg_size, throughput,
                                        latency);
        close_client_sockets (&sockets);
    }
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
    if (!perf_multi_client::parse_endpoint_arg (argc, argv, &endpoint)) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return run_client_benchmark (lib_name, transport, endpoint, fallback_size);
}
