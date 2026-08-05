#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_PUBSUB";
static const zlink_socket_type_t k_client_socket_type = ZLINK_SOCKET_SUB;
static const char *k_pubsub_topic = "bench";

enum pubsub_recv_result_t
{
    pubsub_recv_error = -1,
    pubsub_recv_empty = 0,
    pubsub_recv_payload = 1,
    pubsub_recv_stop = 2
};

using perf_multi_client::close_client_monitors;
using perf_multi_client::close_client_sockets;
using perf_multi_client::create_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::print_client_result_lines;
using perf_multi_client::refresh_connected_client_auto_hwm;
using perf_multi_client::resolve_case_msg_sizes;
using perf_multi_client::wait_client_connect_ready_all;

pubsub_recv_result_t recv_one_pubsub_message (void *socket,
                                              size_t expected_msg_size,
                                              uint32_t expected_run_id,
                                              perf_multi_metric::header_t *header_out,
                                              double *sample_ns_out,
                                              bool *have_sample_out)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    char topic[256];
    size_t topic_len = sizeof (topic);
    if (zlink_msg_init (&part) != 0)
        return pubsub_recv_error;
    const int rc = zlink_subscribe_part (socket, &source_rid, topic, sizeof (topic), &topic_len,
                                         &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return pubsub_recv_empty;
        return pubsub_recv_error;
    }

    if (topic_len != std::strlen (k_pubsub_topic) || source_rid || has_more != ZLINK_PART_FINAL) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] recv shape mismatch topic_len=" << topic_len
                      << " has_more=" << static_cast<int> (has_more) << std::endl;
        }
        zlink_msg_close (&part);
        return pubsub_recv_payload;
    }

    if (is_stop_token_message (part)) {
        zlink_msg_close (&part);
        return pubsub_recv_stop;
    }

    perf_multi_metric::header_t header;
    const bool decoded = perf_multi_metric::decode_payload_header (zlink_msg_data (&part),
                                                                   zlink_msg_size (&part), &header);
    zlink_msg_close (&part);

    if (!decoded || header.magic != perf_multi_metric::k_magic || header.run_id != expected_run_id
        || header.msg_size != static_cast<uint32_t> (expected_msg_size)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] header mismatch decoded=" << decoded
                      << " magic=" << header.magic << " run=" << header.run_id
                      << " phase=" << static_cast<unsigned int> (header.phase)
                      << " size=" << header.msg_size << " expected_size=" << expected_msg_size
                      << std::endl;
        }
        return pubsub_recv_payload;
    }

    if (header_out)
        *header_out = header;
    if (have_sample_out)
        *have_sample_out = false;
    if (sample_ns_out)
        *sample_ns_out = 0.0;
    const uint64_t now_ns = perf_multi_metric::now_ns ();
    if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
        if (sample_ns_out)
            *sample_ns_out = static_cast<double> (now_ns - header.sent_ts_ns);
        if (have_sample_out)
            *have_sample_out = true;
    }
    return pubsub_recv_payload;
}

bool create_pubsub_poller (const std::vector<void *> &sockets, void **poller_out)
{
    if (!poller_out)
        return false;

    *poller_out = NULL;
    void *poller = zlink_poller_new ();
    if (!poller)
        return false;

    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!sockets[i])
            continue;
        if (zlink_poller_add (poller, sockets[i], sockets[i], ZLINK_POLLIN) != 0) {
            zlink_poller_destroy (&poller);
            return false;
        }
    }

    *poller_out = poller;
    return true;
}

bool run_recv_duration (const std::vector<void *> &sockets,
                        void *poller,
                        const multi_bench_settings_t &settings,
                        size_t msg_size,
                        uint32_t run_id,
                        double *throughput_out,
                        bench_latency_stats_t *latency_out)
{
    if (!throughput_out || !latency_out || sockets.empty () || !poller)
        return false;

    const double active_seconds = static_cast<double> (std::max (1, settings.duration_seconds));
    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;
    const auto active_deadline = std::chrono::steady_clock::now ()
                                 + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                                   std::chrono::duration<double> (active_seconds));
    std::vector<zlink_poller_event_t> events (sockets.size ());

    bool phase_done = false;
    while (!phase_done) {
        const int poll_rc = zlink_poller_wait (poller, events.empty () ? NULL : &events[0],
                                               static_cast<int> (events.size ()), -1, NULL);
        if (poll_rc < 0) {
            const int err = zlink_errno ();
            if (err == EINTR || err == EAGAIN)
                continue;
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] poller wait failed err=" << err << std::endl;
            }
            return false;
        }

        for (int i = 0; i < poll_rc; ++i) {
            if ((events[i].events & ZLINK_POLLIN) == 0)
                continue;

            void *socket = events[i].socket;
            while (socket) {
                perf_multi_metric::header_t header;
                std::memset (&header, 0, sizeof (header));
                double sample_ns = 0.0;
                bool have_sample = false;
                const pubsub_recv_result_t recv_rc = recv_one_pubsub_message (
                  socket, msg_size, run_id, &header, &sample_ns, &have_sample);
                if (recv_rc == pubsub_recv_error) {
                    if (bench_debug_enabled ()) {
                        std::cerr << "[multi-pubsub-client] recv error err=" << zlink_errno ()
                                  << std::endl;
                    }
                    return false;
                }
                if (recv_rc == pubsub_recv_empty)
                    break;
                if (recv_rc == pubsub_recv_stop) {
                    phase_done = true;
                    continue;
                }

                if (header.phase == static_cast<uint8_t> (perf_multi_metric::phase_cooldown)) {
                    phase_done = true;
                    continue;
                }
                if (header.phase != static_cast<uint8_t> (perf_multi_metric::phase_active)
                    || std::chrono::steady_clock::now () >= active_deadline) {
                    continue;
                }

                ++recv_count;
                if (have_sample) {
                    lat_sum += sample_ns;
                    ++lat_count;
                    lat_samples.add (sample_ns);
                }
            }
        }
    }

    if (recv_count < 0 || lat_count < 0) {
        if (bench_debug_enabled ()) {
            int events = 0;
            size_t events_size = sizeof (events);
            if (zlink_get_option (sockets[0], ZLINK_OPT_EVENTS, &events, &events_size) != 0) {
                events = -1;
            }
            std::cerr << "[multi-pubsub-client] recv metrics invalid recv=" << recv_count
                      << " lat=" << lat_count << " events=" << events << std::endl;
        }
        return false;
    }

    *throughput_out = throughput_per_second (
      static_cast<uint64_t> (recv_count),
      static_cast<double> (std::max (1, settings.duration_seconds)));
    if (bench_debug_enabled ()) {
        std::cerr << "[multi-pubsub-client] active recv_count=" << recv_count
                  << " lat_count=" << lat_count << std::endl;
    }
    perf_multi_client::normalize_latency_stats (lat_sum, lat_count, &lat_samples, latency_out);
    return true;
}

inline bool run_single_size_case (const std::vector<void *> &sockets,
                                  void *poller,
                                  const multi_bench_settings_t &base_settings,
                                  size_t scratch_capacity,
                                  const std::string &lib_name,
                                  const std::string &transport,
                                  size_t msg_size,
                                  uint32_t run_id)
{
    (void) scratch_capacity;
    if (!perf_multi_handshake::wait_for_start_from_stdin (msg_size))
        return false;

    double throughput = 0.0;
    bench_latency_stats_t latency;
    const bool ok =
      run_recv_duration (sockets, poller, base_settings, msg_size, run_id, &throughput, &latency);
    if (!ok) {
        return false;
    }

    print_client_result_lines (k_pattern, lib_name, transport, msg_size, throughput, latency);

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

    const multi_bench_settings_t base_settings = resolve_multi_bench_settings ();
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
    void *poller = NULL;
    if (!create_client_sockets (ctx, transport, endpoint, base_settings, k_client_socket_type,
                                max_msg_size, &sockets, &monitors, false)) {
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    if (!wait_client_connect_ready_all (monitors, base_settings.connect_ready_timeout_ms)) {
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    close_client_monitors (&monitors);

    if (!create_pubsub_poller (sockets, &poller)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] poller create failed" << std::endl;
        }
        close_client_sockets (&sockets);
        return 1;
    }

    const size_t scratch_capacity = static_cast<size_t> (64);

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];
        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        refresh_connected_client_auto_hwm (ctx.get (), sockets, k_client_socket_type,
                                           base_settings.hwm, transport, msg_size);
        std::cout << "CLIENT_READY," << msg_size << std::endl;
        if (!run_single_size_case (sockets, poller, base_settings, scratch_capacity, lib_name,
                                   transport, msg_size, run_id)) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] size case failed size=" << msg_size
                          << " run_id=" << run_id << std::endl;
            }
            zlink_poller_destroy (&poller);
            close_client_sockets (&sockets);
            return 1;
        }
        std::cout << "CLIENT_DONE," << msg_size << std::endl;
    }
    zlink_poller_destroy (&poller);
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
