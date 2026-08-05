#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include "../common/perf_multi_socket_reqrep.hpp"
#include "../common/perf_multi_weighted_latency.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PERF_MATCHED_PATTERN_NAME
#define PERF_MATCHED_PATTERN_NAME "MULTI_ROUTER_ROUTER_REQREP"
#endif

#if !defined(_WIN32)

namespace
{

const char *const k_pattern = PERF_MATCHED_PATTERN_NAME;
const char *const k_server_routing_id = "SERVER";
const size_t k_child_sample_cap = 1024;

bool is_reqrep ()
{
    return std::string (k_pattern).find ("REQREP") != std::string::npos;
}

struct child_command_t
{
    uint32_t stop;
    uint32_t run_id;
    uint64_t msg_size;
    uint32_t duration_seconds;
};

struct child_result_t
{
    int32_t status;
    uint32_t sample_count;
    uint64_t received;
    double latency_sum_ns;
    double samples[k_child_sample_cap];
};

struct child_slot_t
{
    pid_t pid;
    int command_fd;
    int result_fd;
};

bool write_full (int fd, const void *data, size_t size)
{
    const unsigned char *cursor = static_cast<const unsigned char *> (data);
    while (size > 0) {
        const ssize_t written = write (fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;
        cursor += written;
        size -= static_cast<size_t> (written);
    }
    return true;
}

bool read_full (int fd, void *data, size_t size)
{
    unsigned char *cursor = static_cast<unsigned char *> (data);
    while (size > 0) {
        const ssize_t received = read (fd, cursor, size);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (received == 0)
            return false;
        cursor += received;
        size -= static_cast<size_t> (received);
    }
    return true;
}

bool configure_client_socket (void *ctx,
                              void *socket,
                              int index,
                              const std::string &transport,
                              const std::string &endpoint,
                              const multi_bench_settings_t &settings,
                              size_t max_msg_size)
{
    if (!ctx || !socket)
        return false;
    apply_benchmark_socket_options (
      socket, settings.hwm, transport, ZLINK_SOCKET_ROUTER, max_msg_size, false);
    char routing_id[32];
    const int routing_id_size =
      std::snprintf (routing_id, sizeof (routing_id), "peer-%d", index);
    if (routing_id_size <= 0
        || zlink_set_routing_id (
             socket, routing_id, static_cast<size_t> (routing_id_size))
             != ZLINK_CONFIG_OK
        || zlink_set_router_option (
             socket, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
             k_server_routing_id, std::strlen (k_server_routing_id))
             != 0
        || !setup_tls_client (socket, transport)) {
        return false;
    }

    ready_monitor_t monitor;
    const bool monitor_opened = open_configured_socket_monitor (
      socket,
      ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED
        | ZLINK_EVENT_ACCEPT_FAILED | ZLINK_EVENT_CLOSE_FAILED
        | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
        | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
        | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH,
      &monitor);
    const bool connected =
      monitor_opened
      && zlink_connect (socket, endpoint.c_str ()) == ZLINK_CONNECT_OK
      && wait_for_socket_monitor_event (
        monitor, ZLINK_EVENT_CONNECTION_READY,
        std::max (30000, settings.connect_ready_timeout_ms));
    close_ready_monitor (monitor);
    return connected;
}

bool run_reqrep_case (
  perf_multi_socket_reqrep::client_state_t *state,
  const multi_bench_settings_t &settings,
  const child_command_t &command,
  child_result_t *result)
{
    unsigned long long received = 0;
    bench_latency_stats_t latency;
    perf_multi_socket_reqrep::endpoint_config_t config;
    config.pattern_name = k_pattern;
    config.client_socket_type = ZLINK_SOCKET_ROUTER;
    config.server_socket_type = ZLINK_SOCKET_ROUTER;
    config.client_router_request = true;
    config.server_has_routing_id = true;
    config.server_routing_id = k_server_routing_id;
    if (!perf_multi_socket_reqrep::run_active_window (
          config, state, settings, command.run_id,
          static_cast<size_t> (command.msg_size), &received, &latency)) {
        return false;
    }
    std::vector<double> samples;
    state->latency.append_samples (&samples);
    result->received = received;
    result->latency_sum_ns = state->latency.sum_ns ();
    result->sample_count = static_cast<uint32_t> (
      std::min (samples.size (), k_child_sample_cap));
    for (size_t i = 0; i < result->sample_count; ++i)
        result->samples[i] = samples[i];
    return received > 0;
}

bool run_sendsend_case (
  void *socket,
  const std::string &transport,
  const multi_bench_settings_t &settings,
  const child_command_t &command,
  child_result_t *result)
{
    const size_t msg_size = static_cast<size_t> (command.msg_size);
    const size_t payload_size =
      std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    std::vector<char> payload (payload_size, 'c');
    std::vector<void *> sockets (1, socket);
    long received = 0;
    double latency_sum = 0.0;
    long latency_count = 0;
    bench_latency_stats_t latency;
    std::vector<double> samples;
    if (!perf_multi_client::run_echo_window_round_robin (
          sockets, settings, payload, payload_size, msg_size,
          k_server_routing_id, true, command.run_id,
          perf_multi_metric::phase_active,
          perf_multi_client::metric_capture_bytes (),
          static_cast<double> (
            std::max<uint32_t> (1, command.duration_seconds)),
          true, true, transport == "tcp", &received, &latency_sum,
          &latency_count, &latency, &samples)) {
        return false;
    }
    result->received = static_cast<uint64_t> (std::max<long> (0, received));
    result->latency_sum_ns = latency_sum;
    result->sample_count = static_cast<uint32_t> (
      std::min (samples.size (), k_child_sample_cap));
    for (size_t i = 0; i < result->sample_count; ++i)
        result->samples[i] = samples[i];
    return received > 0 && latency_count > 0;
}

int child_main (int index,
                int command_fd,
                int result_fd,
                const std::string &transport,
                const std::string &endpoint,
                size_t max_msg_size)
{
    int ready_status = 1;
    ctx_guard_t ctx;
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    if (!ctx.valid ()
        || zlink_ctx_set (ctx.get (), ZLINK_IO_THREADS, 1) != 0) {
        write_full (result_fd, &ready_status, sizeof (ready_status));
        return 10;
    }
    void *socket = zlink_socket (ctx.get (), ZLINK_SOCKET_ROUTER);
    if (!configure_client_socket (
          ctx.get (), socket, index, transport, endpoint, settings,
          max_msg_size)) {
        write_full (result_fd, &ready_status, sizeof (ready_status));
        if (socket)
            zlink_close (socket);
        return 11;
    }

    perf_multi_socket_reqrep::client_state_t reqrep_state;
    if (is_reqrep ()) {
        reqrep_state.slots.resize (1);
        reqrep_state.slots[0].owner = &reqrep_state;
        reqrep_state.slots[0].socket = socket;
        reqrep_state.slots[0].index = 0;
        reqrep_state.poller = zlink_poller_new ();
        if (!reqrep_state.poller
            || zlink_poller_add (
                 reqrep_state.poller, socket, &reqrep_state.slots[0],
                 ZLINK_POLLCOMPLETION)
                 != 0) {
            write_full (result_fd, &ready_status, sizeof (ready_status));
            if (reqrep_state.poller)
                zlink_poller_destroy (&reqrep_state.poller);
            zlink_close (socket);
            return 12;
        }
        reqrep_state.events.resize (1);
    }

    ready_status = 0;
    if (!write_full (result_fd, &ready_status, sizeof (ready_status)))
        return 13;

    for (;;) {
        child_command_t command;
        if (!read_full (command_fd, &command, sizeof (command))
            || command.stop) {
            break;
        }
        child_result_t result;
        std::memset (&result, 0, sizeof (result));
        const size_t msg_size = static_cast<size_t> (command.msg_size);
        const bool prepared =
          apply_benchmark_context_auto_hwm_msg_unit (ctx.get (), msg_size);
        apply_benchmark_hwm (socket, settings.hwm);
        const bool recalculated =
          prepared && zlink_ctx_auto_hwm_recalculate (ctx.get ()) == ZLINK_CONFIG_OK;
        const bool ran =
          recalculated
          && (is_reqrep ()
                ? run_reqrep_case (
                    &reqrep_state, settings, command, &result)
                : run_sendsend_case (
                    socket, transport, settings, command, &result));
        result.status = ran ? 0 : 1;
        if (!write_full (result_fd, &result, sizeof (result)))
            break;
    }

    if (reqrep_state.poller)
        zlink_poller_destroy (&reqrep_state.poller);
    zlink_close (socket);
    return 0;
}

int run_client (const std::string &lib_name,
                const std::string &transport,
                const std::string &endpoint,
                size_t fallback_size)
{
    set_perf_multi_pattern_env (k_pattern);
    signal (SIGPIPE, SIG_IGN);
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> sizes =
      perf_multi_client::resolve_case_msg_sizes (fallback_size);
    const size_t max_msg_size =
      perf_multi_client::resolve_case_max_msg_size (fallback_size, sizes);
    std::vector<child_slot_t> children (settings.clients);

    for (size_t i = 0; i < children.size (); ++i) {
        int commands[2];
        int results[2];
        if (pipe (commands) != 0 || pipe (results) != 0)
            return 1;
        const pid_t pid = fork ();
        if (pid < 0)
            return 1;
        if (pid == 0) {
            close (commands[1]);
            close (results[0]);
            const int rc = child_main (
              static_cast<int> (i), commands[0], results[1],
              transport, endpoint, max_msg_size);
            close (commands[0]);
            close (results[1]);
            std::_Exit (rc);
        }
        close (commands[0]);
        close (results[1]);
        children[i].pid = pid;
        children[i].command_fd = commands[1];
        children[i].result_fd = results[0];
    }

    bool ok = true;
    for (size_t i = 0; i < children.size (); ++i) {
        int ready_status = 1;
        if (!read_full (
              children[i].result_fd, &ready_status,
              sizeof (ready_status))
            || ready_status != 0) {
            ok = false;
        }
    }

    for (size_t si = 0; si < sizes.size () && ok; ++si) {
        const size_t msg_size = sizes[si];
        std::cout << "CLIENT_READY," << msg_size << std::endl;
        if (!perf_multi_handshake::wait_for_start_from_stdin (msg_size)) {
            ok = false;
            break;
        }
        child_command_t command;
        std::memset (&command, 0, sizeof (command));
        command.run_id = static_cast<uint32_t> (si + 1);
        command.msg_size = msg_size;
        command.duration_seconds =
          static_cast<uint32_t> (std::max (1, settings.duration_seconds));
        for (size_t i = 0; i < children.size (); ++i) {
            if (!write_full (
                  children[i].command_fd, &command, sizeof (command))) {
                ok = false;
                break;
            }
        }

        uint64_t total_received = 0;
        double total_latency = 0.0;
        std::vector<perf_multi_latency::weighted_sample_t> samples;
        samples.reserve (children.size () * k_child_sample_cap);
        for (size_t i = 0; i < children.size () && ok; ++i) {
            child_result_t result;
            std::memset (&result, 0, sizeof (result));
            if (!read_full (
                  children[i].result_fd, &result, sizeof (result))
                || result.status != 0) {
                ok = false;
                break;
            }
            total_received += result.received;
            total_latency += result.latency_sum_ns;
            if (result.sample_count > 0) {
                const double weight =
                  static_cast<double> (result.received)
                  / static_cast<double> (result.sample_count);
                for (size_t s = 0; s < result.sample_count; ++s) {
                    perf_multi_latency::weighted_sample_t sample;
                    sample.value = result.samples[s];
                    sample.weight = weight;
                    samples.push_back (sample);
                }
            }
        }
        if (!ok || total_received == 0)
            break;

        const bench_latency_stats_t latency =
          perf_multi_latency::aggregate (
            total_received, total_latency, &samples);
        print_result (
          lib_name, k_pattern, transport, msg_size,
          throughput_per_second (
            total_received,
            static_cast<double> (
              std::max (1, settings.duration_seconds))),
          latency.mean_ns, latency.p95_ns, latency.p99_ns);
        std::cout << "MATCHED_DIAG," << lib_name << "," << k_pattern
                  << "," << transport << "," << msg_size
                  << ",role=peers,processes=" << children.size ()
                  << ",contexts=" << children.size ()
                  << ",sockets=" << children.size ()
                  << ",received=" << total_received << std::endl;
        std::cout << "CLIENT_DONE," << msg_size << std::endl;
    }

    child_command_t stop;
    std::memset (&stop, 0, sizeof (stop));
    stop.stop = 1;
    for (size_t i = 0; i < children.size (); ++i) {
        (void) write_full (
          children[i].command_fd, &stop, sizeof (stop));
        close (children[i].command_fd);
        close (children[i].result_fd);
    }
    for (size_t i = 0; i < children.size (); ++i) {
        int status = 0;
        waitpid (children[i].pid, &status, 0);
        if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
            ok = false;
    }
    return ok ? 0 : 1;
}

}

#endif

int main (int argc, char **argv)
{
#if defined(_WIN32)
    (void) argc;
    (void) argv;
    return 1;
#else
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;
    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t fallback_size =
      static_cast<size_t> (std::strtoull (argv[3], NULL, 10));
    std::string endpoint;
    if (!perf_multi_client::parse_endpoint_arg (
          argc, argv, &endpoint)) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }
    return run_client (
      lib_name, transport, endpoint, fallback_size);
#endif
}
