#include "../common/bench_router_compare_common.hpp"

#include <zmq.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
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
    const int rcvhwm = static_cast<int> (parse_long_env ("BENCH_HWM", 1000, 1));
    const int sndhwm = static_cast<int> (parse_long_env ("BENCH_CLIENT_SNDHWM", 10, 1));
    (void) zmq_setsockopt (socket, ZMQ_LINGER, &linger, sizeof (linger));
    (void) zmq_setsockopt (socket, ZMQ_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo));
    (void) zmq_setsockopt (socket, ZMQ_SNDTIMEO, &sndtimeo, sizeof (sndtimeo));
    (void) zmq_setsockopt (socket, ZMQ_RCVHWM, &rcvhwm, sizeof (rcvhwm));
    (void) zmq_setsockopt (socket, ZMQ_SNDHWM, &sndhwm, sizeof (sndhwm));
#ifdef ZMQ_TCP_NODELAY
    const int nodelay = 1;
    (void) zmq_setsockopt (socket, ZMQ_TCP_NODELAY, &nodelay, sizeof (nodelay));
#endif
}

bool send_rtt_message (void *socket, std::vector<unsigned char> &payload, uint64_t send_ts)
{
    store_u64_be (payload.data (), send_ts);
    store_u64_be (payload.data () + 8, send_ts ^ 0x5a5a5a5a5a5a5a5aULL);

    if (zmq_send (socket, k_server_routing_id, std::strlen (k_server_routing_id), ZMQ_SNDMORE)
        < 0) {
        return false;
    }
    return zmq_send (socket, payload.data (), payload.size (), 0) >= 0;
}

bool recv_rtt_message (void *socket,
                       char *id_buf,
                       size_t id_cap,
                       unsigned char *payload_buf,
                       size_t payload_cap,
                       uint64_t &wire_send_ts)
{
    const int id_len = zmq_recv (socket, id_buf, id_cap, ZMQ_DONTWAIT);
    if (id_len < 0)
        return false;
    const int rc = zmq_recv (socket, payload_buf, payload_cap, 0);
    if (rc < 16)
        return false;
    wire_send_ts = load_u64_be (payload_buf);
    return true;
}

void close_all (std::vector<void *> &sockets, void *ctx)
{
    for (size_t i = 0; i < sockets.size (); ++i)
        if (sockets[i])
            zmq_close (sockets[i]);
    zmq_ctx_term (ctx);
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
    std::vector<zmq_pollitem_t> poll_items (static_cast<size_t> (clients));
    for (int i = 0; i < clients; ++i) {
        poll_items[static_cast<size_t> (i)].socket = sockets[static_cast<size_t> (i)];
        poll_items[static_cast<size_t> (i)].fd = 0;
        poll_items[static_cast<size_t> (i)].events = ZMQ_POLLIN | ZMQ_POLLOUT;
        poll_items[static_cast<size_t> (i)].revents = 0;
    }

    while (std::chrono::steady_clock::now () < measure_end) {
        const int poll_rc = zmq_poll (poll_items.data (), clients, 1);
        if (poll_rc <= 0)
            continue;

        for (int i = 0; i < clients; ++i) {
            const short rev = poll_items[static_cast<size_t> (i)].revents;

            // Writable: send one message (POLLOUT means HWM has room)
            if (rev & ZMQ_POLLOUT) {
                send_rtt_message (sockets[static_cast<size_t> (i)], payload, now_ns ());
            }

            // Readable: batch-drain all available responses
            if (rev & ZMQ_POLLIN) {
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
        poll_items[static_cast<size_t> (i)].events = ZMQ_POLLIN;

    const auto drain_end = std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_ms);
    while (std::chrono::steady_clock::now () < drain_end) {
        const int poll_rc = zmq_poll (poll_items.data (), clients, 1);
        if (poll_rc <= 0)
            continue;

        for (int i = 0; i < clients; ++i) {
            if (!(poll_items[static_cast<size_t> (i)].revents & ZMQ_POLLIN))
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
    const std::string lib_name = argc > 1 ? std::string (argv[1]) : std::string ("libzmq");
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

    void *ctx = zmq_ctx_new ();
    if (!ctx) {
        std::fprintf (stderr, "rc client: zmq_ctx_new failed\n");
        return 2;
    }
    (void) zmq_ctx_set (ctx, ZMQ_IO_THREADS, io_threads);
    const long max_sockets_default = std::max<long> (2048, clients + 1024L);
    const int max_sockets =
      static_cast<int> (parse_long_env ("BENCH_MAX_SOCKETS", max_sockets_default, 1));
    (void) zmq_ctx_set (ctx, ZMQ_MAX_SOCKETS, max_sockets);

    const std::string endpoint = endpoint_from_port (port);

    std::vector<void *> sockets (static_cast<size_t> (clients), NULL);

    for (int i = 0; i < clients; ++i) {
        void *sock = zmq_socket (ctx, ZMQ_ROUTER);
        if (!sock) {
            std::fprintf (stderr, "rc client: zmq_socket failed at index=%d errno=%d\n", i,
                          zmq_errno ());
            close_all (sockets, ctx);
            return 2;
        }

        apply_socket_options (sock);

        char rid[64];
        std::snprintf (rid, sizeof (rid), "RC_C_%d", i);
        (void) zmq_setsockopt (sock, ZMQ_ROUTING_ID, rid, std::strlen (rid));

        if (zmq_connect (sock, endpoint.c_str ()) != 0) {
            std::fprintf (stderr, "rc client: zmq_connect failed at index=%d errno=%d\n", i,
                          zmq_errno ());
            zmq_close (sock);
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
