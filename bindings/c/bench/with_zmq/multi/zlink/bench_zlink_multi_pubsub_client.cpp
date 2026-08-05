#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "bench_multi_resource.hpp"

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
static const int k_client_socket_type = ZLINK_SOCKET_SUB;
static const uint32_t k_metric_run_id = 1U;
static const char *k_pubsub_topic = "bench";

using perf_multi_client::close_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::print_client_result_lines;
using perf_multi_client::resolve_case_msg_sizes;

int recv_one_pubsub_message (void *socket,
                             size_t expected_msg_size,
                             uint32_t expected_run_id,
                             perf_multi_metric::header_t *header_out,
                             double *sample_us_out,
                             bool *have_sample_out)
{
    size_t topic_len = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const int rc =
      zlink_std_compat_subscribe (socket, &parts, &part_count, ZLINK_DONTWAIT, NULL, &topic_len);
    if (rc != 0) {
        const int err = zlink_errno ();
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    if (topic_len != std::strlen (k_pubsub_topic) || !parts || part_count == 0) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] recv shape mismatch topic_len=" << topic_len
                      << " part_count=" << part_count << std::endl;
        }
        if (parts) {
            zlink_multipart_close (parts, part_count);
        }
        return 1;
    }

    perf_multi_metric::header_t header;
    const bool decoded = perf_multi_metric::decode_payload_header (
      zlink_msg_data (&parts[0]), zlink_msg_size (&parts[0]), &header);
    zlink_multipart_close (parts, part_count);

    if (!decoded || header.magic != perf_multi_metric::k_magic || header.run_id != expected_run_id
        || header.msg_size != static_cast<uint32_t> (expected_msg_size)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] header mismatch decoded=" << decoded
                      << " magic=" << header.magic << " run=" << header.run_id
                      << " phase=" << header.phase << " size=" << header.msg_size
                      << " expected_size=" << expected_msg_size << std::endl;
        }
        return 1;
    }

    if (header_out)
        *header_out = header;
    if (have_sample_out)
        *have_sample_out = false;
    if (sample_us_out)
        *sample_us_out = 0.0;
    const uint64_t now_us = perf_multi_metric::now_us ();
    if (header.sent_ts_us > 0 && now_us >= header.sent_ts_us) {
        if (sample_us_out)
            *sample_us_out = static_cast<double> (now_us - header.sent_ts_us);
        if (have_sample_out)
            *have_sample_out = true;
    }
    return 1;
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
                        bench_latency_stats_t *latency_out,
                        bench_multi_resource_metrics_t *metrics_out)
{
    if (!throughput_out || !latency_out || !metrics_out || sockets.empty () || !poller)
        return false;

    const double active_seconds = static_cast<double> (std::max (1, settings.duration_seconds));
    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;
    const double prelude_seconds = static_cast<double> (std::max (0, settings.warmup_seconds));
    const auto start_wait_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (
          prelude_seconds
          + static_cast<double> (std::max (1, settings.connect_ready_timeout_ms)) / 1000.0));

    bool active_started = false;
    bench_multi_cpu_sample_t sample_start;
    auto active_deadline = std::chrono::steady_clock::time_point ();
    std::vector<zlink_poller_event_t> events (sockets.size ());

    while (true) {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
        const std::chrono::steady_clock::time_point deadline =
          active_started ? active_deadline : start_wait_deadline;
        if (now >= deadline)
            break;

        const int timeout_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ());
        const int poll_rc = zlink_poller_wait (poller, events.empty () ? NULL : &events[0],
                                               static_cast<int> (events.size ()),
                                               timeout_ms > 5 ? 5 : std::max (1, timeout_ms));
        if (poll_rc < 0) {
            const int err = zlink_errno ();
            if (err == EINTR || err == EAGAIN)
                continue;
            return false;
        }

        bool progressed = false;
        for (int i = 0; i < poll_rc; ++i) {
            if ((events[i].events & ZLINK_POLLIN) == 0)
                continue;

            void *socket = events[i].socket;
            while (socket) {
                perf_multi_metric::header_t header;
                std::memset (&header, 0, sizeof (header));
                double sample_us = 0.0;
                bool have_sample = false;
                const int recv_rc = recv_one_pubsub_message (socket, msg_size, run_id, &header,
                                                             &sample_us, &have_sample);
                if (recv_rc < 0)
                    return false;
                if (recv_rc == 0)
                    break;

                progressed = true;
                if (header.phase != static_cast<uint32_t> (perf_multi_metric::phase_active)) {
                    continue;
                }

                if (!active_started) {
                    active_started = true;
                    recv_count = 0;
                    lat_sum = 0.0;
                    lat_count = 0;
                    lat_samples = bench_latency_sampler_t ();
                    sample_start = bench_multi_capture_cpu_sample ();
                    active_deadline =
                      std::chrono::steady_clock::now ()
                      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                        std::chrono::duration<double> (active_seconds));
                }

                ++recv_count;
                if (have_sample) {
                    lat_sum += sample_us;
                    ++lat_count;
                    lat_samples.add (sample_us);
                }
            }
        }

        if (!progressed && perf_socket_poll (NULL, 0, 1) < 0 && zlink_errno () != EINTR) {
            return false;
        }
    }

    if (!active_started)
        return false;

    *metrics_out = bench_multi_finish_resource_probe (sample_start);

    if (recv_count <= 0 || lat_count <= 0) {
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

    *throughput_out = static_cast<double> (recv_count)
                      / static_cast<double> (std::max (1, settings.duration_seconds));
    perf_multi_client::normalize_latency_stats (lat_sum, lat_count, &lat_samples, latency_out);
    return true;
}

inline bool create_client_sockets (ctx_guard_t &ctx,
                                   const std::string &transport,
                                   const std::string &endpoint,
                                   const multi_bench_settings_t &settings,
                                   std::vector<void *> *sockets_out)
{
    if (!sockets_out)
        return false;

    sockets_out->assign (settings.clients, NULL);

    for (size_t i = 0; i < sockets_out->size (); ++i) {
        void *sock =
          zlink_socket (ctx.get (), static_cast<zlink_socket_type_t> (k_client_socket_type));
        if (!sock) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] socket create failed slot=" << i
                          << " errno=" << zlink_errno () << std::endl;
            }
            return false;
        }

        apply_benchmark_socket_options (sock, settings.hwm, transport);
        static const char k_subscribe_all[] = "";
        if (!zlink_set_subscription (sock, k_subscribe_all)
            || !setup_tls_client (sock, transport)) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] subscribe/tls failed slot=" << i
                          << " errno=" << zlink_errno () << std::endl;
            }
            zlink_close (sock);
            return false;
        }

        if (zlink_connect (sock, endpoint.c_str ()) != ZLINK_CONNECT_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] connect failed slot=" << i
                          << " endpoint=" << endpoint << " errno=" << zlink_errno () << std::endl;
            }
            zlink_close (sock);
            return false;
        }

        (*sockets_out)[i] = sock;
    }

    return true;
}

inline bool run_single_size_case (const std::vector<void *> &sockets,
                                  void *poller,
                                  const multi_bench_settings_t &base_settings,
                                  size_t scratch_capacity,
                                  const std::string &lib_name,
                                  const std::string &transport,
                                  size_t msg_size)
{
    double throughput = 0.0;
    bench_latency_stats_t latency;
    bench_multi_resource_metrics_t metrics;
    const bool ok = run_recv_duration (sockets, poller, base_settings, msg_size, k_metric_run_id,
                                       &throughput, &latency, &metrics);
    if (!ok) {
        return false;
    }

    print_client_result_lines (k_pattern, lib_name, transport, msg_size, throughput, latency,
                               metrics);

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

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    std::vector<void *> sockets;
    void *poller = NULL;
    if (!create_client_sockets (ctx, transport, endpoint, base_settings, &sockets)) {
        close_client_sockets (&sockets);
        return 1;
    }

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
        std::cout << "CLIENT_READY," << msg_size << std::endl;
        if (!run_single_size_case (sockets, poller, base_settings, scratch_capacity, lib_name,
                                   transport, msg_size)) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] size case failed size=" << msg_size
                          << std::endl;
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
