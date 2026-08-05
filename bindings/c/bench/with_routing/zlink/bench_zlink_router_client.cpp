#include "../common/bench_router_compare_common.hpp"

#include <zlink.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace bench_rc;

static const char k_server_routing_id[] = "RC_SRV";

void apply_socket_options (void *socket)
{
    const int linger = 0;
    const int rcvtimeo = 100;
    const int sndtimeo = 100;
    const uint64_t rcvhwm =
      static_cast<uint64_t> (parse_long_env ("BENCH_HWM", 1000, 1));
    const uint64_t sndhwm =
      static_cast<uint64_t> (parse_long_env ("BENCH_CLIENT_SNDHWM", 10, 1));
    (void) zlink_set_option (socket, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    (void) zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo));
    (void) zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &sndtimeo, sizeof (sndtimeo));
    if (zlink_set_option (socket, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)) != 0
        || zlink_set_option (socket, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)) != 0)
        std::abort ();
    const int nodelay = 1;
    (void) zlink_set_option (socket, ZLINK_OPT_TCP_NODELAY, &nodelay, sizeof (nodelay));
}

bool send_rtt_message (void *socket, std::vector<unsigned char> &payload, uint64_t send_ts)
{
    store_u64_be (payload.data (), send_ts);
    store_u64_be (payload.data () + 8, send_ts ^ 0x5a5a5a5a5a5a5a5aULL);

    zlink_routing_id_t target_rid;
    const size_t server_id_size = std::strlen (k_server_routing_id);
    target_rid.size = static_cast<uint8_t> (std::min (server_id_size, sizeof (target_rid.data)));
    if (target_rid.size > 0)
        std::memcpy (target_rid.data, k_server_routing_id, target_rid.size);

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload.size ()) != 0)
        return false;
    if (!payload.empty ())
        std::memcpy (zlink_msg_data (&part), payload.data (), payload.size ());
    if (::zlink_send_part_rid (socket, &target_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL)
        != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&part);
        return false;
    }
    return true;
}

bool recv_rtt_message (void *socket,
                       char *id_buf,
                       size_t id_cap,
                       unsigned char *payload_buf,
                       size_t payload_cap,
                       uint64_t &wire_send_ts)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return false;
    const zlink_recv_result_t rc =
      ::zlink_recv_part (socket, &source_rid, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&part);
        return false;
    }

    if (!source_rid || source_rid->size == 0) {
        zlink_msg_close (&part);
        return false;
    }

    const size_t id_len = std::min (id_cap, static_cast<size_t> (source_rid->size));
    std::memcpy (id_buf, source_rid->data, id_len);

    const size_t payload_size = zlink_msg_size (&part);
    const size_t payload_len = std::min (payload_cap, payload_size);
    if (payload_len > 0) {
        std::memcpy (payload_buf, zlink_msg_data (&part), payload_len);
    }

    while (has_more == ZLINK_PART_MORE) {
        zlink_msg_t next;
        if (zlink_msg_init (&next) != 0) {
            zlink_msg_close (&part);
            return false;
        }
        const zlink_recv_result_t next_rc =
          ::zlink_recv_part (socket, &source_rid, &next, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        zlink_msg_close (&next);
        if (next_rc != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            return false;
        }
    }
    zlink_msg_close (&part);

    if (payload_len < 16)
        return false;
    wire_send_ts = load_u64_be (payload_buf);
    return true;
}

void close_all (std::vector<void *> &sockets, void *ctx)
{
    for (size_t i = 0; i < sockets.size (); ++i)
        if (sockets[i])
            zlink_close (sockets[i]);
    zlink_ctx_term (ctx);
}

bool run_measure_once (const std::vector<void *> &sockets,
                       int clients,
                       int duration_s,
                       int settle_ms,
                       int drain_ms,
                       size_t msg_size,
                       double &throughput_out,
                       double &latency_out)
{
    if (settle_ms > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (settle_ms));

    const auto measure_start = std::chrono::steady_clock::now ();
    const auto measure_end = measure_start + std::chrono::seconds (duration_s);

    long recv_count = 0;
    double latency_sum_us = 0.0;
    long latency_samples = 0;

    std::vector<unsigned char> payload (std::max<size_t> (16, msg_size), 0xAB);

    std::vector<char> recv_id_buf (512);
    std::vector<unsigned char> recv_payload_buf (1024 * 1024);

    // Build poll items: POLLIN | POLLOUT for async pipeline
    std::vector<zlink_pollitem_t> poll_items (static_cast<size_t> (clients));
    for (int i = 0; i < clients; ++i) {
        poll_items[static_cast<size_t> (i)].socket = sockets[static_cast<size_t> (i)];
        poll_items[static_cast<size_t> (i)].fd = 0;
        poll_items[static_cast<size_t> (i)].events = ZLINK_POLLIN | ZLINK_POLLOUT;
        poll_items[static_cast<size_t> (i)].revents = 0;
    }

    while (std::chrono::steady_clock::now () < measure_end) {
        const int poll_rc = zlink_poll (poll_items.data (), clients, 1, NULL);
        if (poll_rc <= 0)
            continue;

        for (int i = 0; i < clients; ++i) {
            const short rev = poll_items[static_cast<size_t> (i)].revents;

            // Writable: send one message (POLLOUT means HWM has room)
            if (rev & ZLINK_POLLOUT) {
                send_rtt_message (sockets[static_cast<size_t> (i)], payload, now_ns ());
            }

            // Readable: batch-drain all available responses
            if (rev & ZLINK_POLLIN) {
                uint64_t wire_ts = 0;
                while (recv_rtt_message (sockets[static_cast<size_t> (i)], recv_id_buf.data (),
                                         recv_id_buf.size (), recv_payload_buf.data (),
                                         recv_payload_buf.size (), wire_ts)) {
                    const uint64_t now = now_ns ();
                    if (now >= wire_ts) {
                        latency_sum_us += static_cast<double> (now - wire_ts) / 1000.0;
                        ++latency_samples;
                    }
                    ++recv_count;
                }
            }
        }
    }

    const auto measure_stop = std::chrono::steady_clock::now ();

    // Drain phase: collect remaining in-flight responses (POLLIN only)
    for (int i = 0; i < clients; ++i)
        poll_items[static_cast<size_t> (i)].events = ZLINK_POLLIN;

    const auto drain_end = std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_ms);
    while (std::chrono::steady_clock::now () < drain_end) {
        const int poll_rc = zlink_poll (poll_items.data (), clients, 1, NULL);
        if (poll_rc <= 0)
            continue;

        for (int i = 0; i < clients; ++i) {
            if (!(poll_items[static_cast<size_t> (i)].revents & ZLINK_POLLIN))
                continue;

            uint64_t wire_ts = 0;
            while (recv_rtt_message (sockets[static_cast<size_t> (i)], recv_id_buf.data (),
                                     recv_id_buf.size (), recv_payload_buf.data (),
                                     recv_payload_buf.size (), wire_ts)) {
                const uint64_t now = now_ns ();
                if (now >= wire_ts) {
                    latency_sum_us += static_cast<double> (now - wire_ts) / 1000.0;
                    ++latency_samples;
                }
                ++recv_count;
            }
        }
    }

    double elapsed_s =
      std::chrono::duration_cast<std::chrono::duration<double>> (measure_stop - measure_start)
        .count ();
    if (elapsed_s <= 0.0)
        elapsed_s = static_cast<double> (std::max (1, duration_s));

    throughput_out = static_cast<double> (recv_count) / elapsed_s;
    latency_out =
      latency_samples > 0 ? latency_sum_us / static_cast<double> (latency_samples) : 0.0;
    return true;
}

} // namespace

int main (int argc, char **argv)
{
    const std::string lib_name = argc > 1 ? std::string (argv[1]) : std::string ("zlink");
    const std::string msg_sizes_raw = parse_string_env ("BENCH_MSG_SIZES", "");
    std::vector<size_t> msg_sizes;
    if (!msg_sizes_raw.empty () && !parse_size_list (msg_sizes_raw, msg_sizes)) {
        std::fprintf (stderr, "rc client: invalid BENCH_MSG_SIZES\n");
        return 2;
    }
    if (msg_sizes.empty ())
        msg_sizes = {64, 256, 1024, 65536, 131072, 262144};

    const int clients = static_cast<int> (parse_long_env ("BENCH_CLIENTS", 100, 1));
    const int duration_s = static_cast<int> (parse_long_env ("BENCH_MULTI_DURATION_SECONDS", 5, 1));
    const int settle_ms = static_cast<int> (parse_long_env ("BENCH_MULTI_SETTLE_MS", 500, 0));
    const int drain_ms = static_cast<int> (parse_long_env ("BENCH_MULTI_DRAIN_MS", 300, 0));
    const int transition_drain_ms = resolve_size_transition_drain_ms (drain_ms);
    const int io_threads = static_cast<int> (parse_long_env ("BENCH_IO_THREADS", 4, 1));
    const int port = static_cast<int> (parse_long_env ("BENCH_PORT", 29200, 1));

    void *ctx = zlink_ctx_new ();
    if (!ctx) {
        std::fprintf (stderr, "rc client: zlink_ctx_new failed\n");
        return 2;
    }
    (void) zlink_ctx_set (ctx, ZLINK_IO_THREADS, io_threads);
    const long max_sockets_default = std::max<long> (2048, clients + 1024L);
    const int max_sockets =
      static_cast<int> (parse_long_env ("BENCH_MAX_SOCKETS", max_sockets_default, 1));
    (void) zlink_ctx_set (ctx, ZLINK_MAX_SOCKETS, max_sockets);

    const std::string endpoint = endpoint_from_port (port);

    std::vector<void *> sockets (static_cast<size_t> (clients), NULL);

    for (int i = 0; i < clients; ++i) {
        void *sock = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
        if (!sock) {
            std::fprintf (stderr, "rc client: zlink_socket failed at index=%d errno=%d\n", i,
                          zlink_errno ());
            close_all (sockets, ctx);
            return 2;
        }

        apply_socket_options (sock);

        char rid[64];
        std::snprintf (rid, sizeof (rid), "RC_C_%d", i);
        (void) zlink_set_routing_id (sock, rid, std::strlen (rid));

        if (zlink_connect (sock, endpoint.c_str ()) != ZLINK_CONNECT_OK) {
            std::fprintf (stderr, "rc client: zlink_connect failed at index=%d errno=%d\n", i,
                          zlink_errno ());
            zlink_close (sock);
            close_all (sockets, ctx);
            return 2;
        }

        sockets[static_cast<size_t> (i)] = sock;
    }

    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        double throughput = 0.0;
        double latency = 0.0;
        if (!run_measure_once (sockets, clients, duration_s, settle_ms, drain_ms, msg_sizes[i],
                               throughput, latency)) {
            close_all (sockets, ctx);
            return 2;
        }
        print_result (lib_name, "tcp", msg_sizes[i], throughput, latency);
        run_size_transition_drain_stage (transition_drain_ms, (i + 1) < msg_sizes.size ());
    }

    close_all (sockets, ctx);
    return 0;
}
