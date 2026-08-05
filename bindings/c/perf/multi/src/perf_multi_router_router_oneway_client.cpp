#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include "../common/perf_multi_weighted_latency.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

namespace
{

const char *const k_pattern = "MULTI_ROUTER_ROUTER_ONEWAY";
const char *const k_server_routing_id = "SERVER";
const size_t k_child_sample_cap = 1024;

#if !defined(_WIN32)

struct child_command_t
{
    uint32_t stop;
    uint32_t run_id;
    uint64_t msg_size;
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

class fixed_reservoir_t
{
  public:
    fixed_reservoir_t () : _seen (0), _rng (0xA341316Cu) {}

    void add (double value)
    {
        ++_seen;
        if (_samples.size () < k_child_sample_cap) {
            _samples.push_back (value);
            return;
        }
        const uint64_t slot = next_random () % _seen;
        if (slot < _samples.size ())
            _samples[static_cast<size_t> (slot)] = value;
    }

    const std::vector<double> &samples () const { return _samples; }

  private:
    uint64_t _seen;
    uint32_t _rng;
    std::vector<double> _samples;

    uint32_t next_random ()
    {
        _rng = (_rng * 1664525u) + 1013904223u;
        return _rng;
    }
};

bool receive_case (void *socket, const child_command_t &command, child_result_t *result)
{
    std::memset (result, 0, sizeof (*result));
    fixed_reservoir_t reservoir;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (30);
    bool cooldown = false;

    while (!cooldown && std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t part;
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        if (zlink_msg_init (&part) != 0)
            return false;
        const zlink_recv_result_t rc =
          zlink_router_recv_part (socket, &source, &request_seq, &part, &more,
                                  ZLINK_RECV_FLAGS_NONE);
        if (rc != ZLINK_RECV_OK) {
            const int err = zlink_errno ();
            zlink_msg_close (&part);
            if (rc == ZLINK_RECV_NO_DATA || err == EAGAIN || err == EINTR) {
                continue;
            }
            return false;
        }

        perf_multi_metric::header_t header;
        std::memset (&header, 0, sizeof (header));
        const bool from_hub =
          source && source->size == std::strlen (k_server_routing_id)
          && std::memcmp (source->data, k_server_routing_id, source->size) == 0;
        const bool decoded =
          from_hub && request_seq == 0 && more == ZLINK_PART_FINAL
          && perf_multi_metric::decode_payload_header (
            zlink_msg_data (&part), zlink_msg_size (&part), &header);
        zlink_msg_close (&part);
        if (!decoded || header.run_id != command.run_id
            || header.msg_size != static_cast<uint32_t> (command.msg_size))
            continue;
        if (header.phase == static_cast<uint8_t> (perf_multi_metric::phase_cooldown)) {
            cooldown = true;
            continue;
        }
        if (header.phase != static_cast<uint8_t> (perf_multi_metric::phase_active))
            continue;
        const uint64_t now = perf_multi_metric::now_ns ();
        if (header.sent_ts_ns == 0 || now < header.sent_ts_ns)
            continue;
        const double latency = static_cast<double> (now - header.sent_ts_ns);
        ++result->received;
        result->latency_sum_ns += latency;
        reservoir.add (latency);
    }

    const std::vector<double> &samples = reservoir.samples ();
    result->sample_count = static_cast<uint32_t> (samples.size ());
    for (size_t i = 0; i < samples.size (); ++i)
        result->samples[i] = samples[i];
    result->status = cooldown && result->received > 0 ? 0 : 1;
    return result->status == 0;
}

int child_main (size_t index,
                int command_fd,
                int result_fd,
                const std::string &transport,
                const std::string &endpoint)
{
    int ready_status = 1;
    void *ctx = zlink_ctx_new ();
    if (!ctx || zlink_ctx_set (ctx, ZLINK_IO_THREADS, 1) != 0) {
        write_full (result_fd, &ready_status, sizeof (ready_status));
        return 10;
    }
    void *socket = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    char rid[32];
    const int rid_size = std::snprintf (rid, sizeof (rid), "peer-%zu", index);
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    if (socket) {
        apply_benchmark_socket_options (
          socket, settings.hwm, transport, ZLINK_SOCKET_ROUTER,
          perf_current_benchmark_max_msg_size (64), false);
    }
    ready_monitor_t monitor;
    const bool configured =
      socket && rid_size > 0
      && zlink_set_routing_id (socket, rid, static_cast<size_t> (rid_size)) == ZLINK_CONFIG_OK
      && zlink_set_router_option (socket, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                  k_server_routing_id, std::strlen (k_server_routing_id))
           == 0
      && setup_tls_client (socket, transport)
      && open_configured_socket_monitor (
        socket,
        ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
          | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
          | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH,
        &monitor)
      && zlink_connect (socket, endpoint.c_str ()) == ZLINK_CONNECT_OK
      && wait_for_socket_monitor_event (
        monitor, ZLINK_EVENT_CONNECTION_READY, 30000);
    close_ready_monitor (monitor);
    if (!configured) {
        write_full (result_fd, &ready_status, sizeof (ready_status));
        if (socket)
            zlink_close (socket);
        zlink_ctx_term (ctx);
        return 11;
    }

    ready_status = 0;
    if (!write_full (result_fd, &ready_status, sizeof (ready_status)))
        return 12;
    for (;;) {
        child_command_t command;
        if (!read_full (command_fd, &command, sizeof (command)) || command.stop)
            break;
        if (!apply_benchmark_context_auto_hwm_msg_unit (
              ctx, static_cast<size_t> (command.msg_size))) {
            return 13;
        }
        apply_benchmark_hwm (socket, settings.hwm);
        if (zlink_ctx_auto_hwm_recalculate (ctx) != ZLINK_CONFIG_OK)
            return 13;
        child_result_t result;
        receive_case (socket, command, &result);
        if (!write_full (result_fd, &result, sizeof (result)))
            return 14;
    }

    zlink_close (socket);
    zlink_ctx_term (ctx);
    return 0;
}

int run_client (const std::string &lib_name,
                const std::string &transport,
                const std::string &endpoint,
                size_t fallback_size)
{
    set_perf_multi_pattern_env (k_pattern);
    if (!perf_multi_client::is_supported_transport (transport)
        || !transport_available (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }
    signal (SIGPIPE, SIG_IGN);
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> sizes =
      perf_multi_client::resolve_case_msg_sizes (fallback_size);
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
            const int rc = child_main (i, commands[0], results[1], transport, endpoint);
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
        int ready = 1;
        if (!read_full (children[i].result_fd, &ready, sizeof (ready)) || ready != 0)
            ok = false;
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
        for (size_t i = 0; i < children.size (); ++i)
            ok = write_full (children[i].command_fd, &command, sizeof (command)) && ok;

        uint64_t total_received = 0;
        double total_latency = 0.0;
        std::vector<perf_multi_latency::weighted_sample_t> samples;
        for (size_t i = 0; i < children.size () && ok; ++i) {
            child_result_t result;
            std::memset (&result, 0, sizeof (result));
            if (!read_full (children[i].result_fd, &result, sizeof (result))
                || result.status != 0) {
                ok = false;
                break;
            }
            total_received += result.received;
            total_latency += result.latency_sum_ns;
            const double weight =
              result.sample_count == 0
                ? 0.0
                : static_cast<double> (result.received)
                    / static_cast<double> (result.sample_count);
            for (size_t sample = 0; sample < result.sample_count; ++sample) {
                perf_multi_latency::weighted_sample_t weighted;
                weighted.value = result.samples[sample];
                weighted.weight = weight;
                samples.push_back (weighted);
            }
        }
        if (!ok || total_received == 0)
            break;
        const bench_latency_stats_t latency =
          perf_multi_latency::aggregate (total_received, total_latency, &samples);
        print_result (
          lib_name, k_pattern, transport, msg_size,
          throughput_per_second (
            total_received, static_cast<double> (std::max (1, settings.duration_seconds))),
          latency.mean_ns, latency.p95_ns, latency.p99_ns);
        std::cout << "CLIENT_DONE," << msg_size << std::endl;
    }

    child_command_t stop;
    std::memset (&stop, 0, sizeof (stop));
    stop.stop = 1;
    for (size_t i = 0; i < children.size (); ++i) {
        write_full (children[i].command_fd, &stop, sizeof (stop));
        close (children[i].command_fd);
        close (children[i].result_fd);
    }
    for (size_t i = 0; i < children.size (); ++i) {
        int status = 0;
        if (waitpid (children[i].pid, &status, 0) < 0 || !WIFEXITED (status)
            || WEXITSTATUS (status) != 0)
            ok = false;
    }
    return ok ? 0 : 1;
}

#endif

} // namespace

int main (int argc, char **argv)
{
#if !defined(_WIN32)
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;
    std::string endpoint;
    if (!perf_multi_client::parse_endpoint_arg (argc, argv, &endpoint))
        return 1;
    return run_client (argv[1], argv[2], endpoint,
                       static_cast<size_t> (std::strtoull (argv[3], NULL, 10)));
#else
    if (argc < 3)
        return 1;
    std::cout << "UNSUPPORTED," << argv[1] << "," << k_pattern << "," << argv[2]
              << ",peer_process_topology_requires_posix" << std::endl;
    return 0;
#endif
}
