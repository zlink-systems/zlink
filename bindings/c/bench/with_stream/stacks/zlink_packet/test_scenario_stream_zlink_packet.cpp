#include "../common/stream_echo_common.hpp"

#include "../../../../../../core/include/zlink.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <string>
#include <thread>

namespace
{

static const size_t k_min_payload_size = 16;
static const size_t k_max_payload_size = 4 * 1024 * 1024;

struct server_options_t
{
    std::string host;
    int port;
    size_t size;
    int sndbuf;
    int rcvbuf;
    int backlog;
    int tcp_nodelay;
    int io_threads;

    server_options_t () :
        host ("0.0.0.0"),
        port (38001),
        size (1024),
        sndbuf (1024 * 1024),
        rcvbuf (1024 * 1024),
        backlog (32768),
        tcp_nodelay (1),
        io_threads (4)
    {
    }
};

static std::atomic<bool> *g_stop_flag = NULL;
class zlink_stream_packet_echo_server_t;
static zlink_stream_packet_echo_server_t *g_server_instance = NULL;

void on_signal (int)
{
    if (g_stop_flag)
        g_stop_flag->store (true, std::memory_order_release);
}

std::string make_endpoint (const std::string &host, int port)
{
    char buf[256];
    std::snprintf (buf, sizeof (buf), "tcp://%s:%d", host.c_str (), port);
    return std::string (buf);
}

void apply_socket_tuning (void *socket, const server_options_t &opt)
{
    (void) zlink_set_option (socket, ZLINK_OPT_SNDBUF, &opt.sndbuf, sizeof (opt.sndbuf));
    (void) zlink_set_option (socket, ZLINK_OPT_RCVBUF, &opt.rcvbuf, sizeof (opt.rcvbuf));
    (void) zlink_set_option (socket, ZLINK_OPT_BACKLOG, &opt.backlog, sizeof (opt.backlog));
    (void) zlink_set_option (socket, ZLINK_OPT_TCP_NODELAY, &opt.tcp_nodelay,
                             sizeof (opt.tcp_nodelay));
    const uint64_t hwm = 100;
    if (zlink_set_option (socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)) != 0
        || zlink_set_option (socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)) != 0)
        std::abort ();
}

bool build_packet_frame (zlink_msg_t *packet_out,
                         const zlink_msg_t *header_part,
                         const zlink_msg_t *body_part)
{
    if (!packet_out || !header_part || !body_part)
        return false;

    const size_t header_size = zlink_msg_size (const_cast<zlink_msg_t *> (header_part));
    const size_t body_size = zlink_msg_size (const_cast<zlink_msg_t *> (body_part));
    const size_t total_size = stream_echo::k_stream_packet_prefix_size + header_size + body_size;
    if (zlink_msg_init_size (packet_out, total_size) != 0)
        return false;

    unsigned char *dst = static_cast<unsigned char *> (zlink_msg_data (packet_out));
    stream_echo::store_u16_be (dst, static_cast<uint16_t> (header_size & 0xFFFFu));
    stream_echo::store_u32_be (dst + 2, static_cast<uint32_t> (body_size));
    if (header_size > 0) {
        std::memcpy (dst + stream_echo::k_stream_packet_prefix_size,
                     zlink_msg_data (const_cast<zlink_msg_t *> (header_part)), header_size);
    }
    if (body_size > 0) {
        std::memcpy (dst + stream_echo::k_stream_packet_prefix_size + header_size,
                     zlink_msg_data (const_cast<zlink_msg_t *> (body_part)), body_size);
    }
    return true;
}

class zlink_stream_packet_echo_server_t
{
  public:
    explicit zlink_stream_packet_echo_server_t (const server_options_t &opt_) :
        opt (opt_),
        ctx (NULL),
        server (NULL),
        recv_msgs (0),
        parse_error (0),
        protocol_error (0),
        send_error (0),
        stop (false)
    {
    }

    ~zlink_stream_packet_echo_server_t () { cleanup (); }

    int run ()
    {
        ctx = zlink_ctx_new ();
        if (!ctx) {
            std::fprintf (stderr, "zlink_packet stream: zlink_ctx_new failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        (void) zlink_ctx_set (ctx, ZLINK_IO_THREADS, opt.io_threads);

        server = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
        if (!server) {
            std::fprintf (stderr, "zlink_packet stream: zlink_socket failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        apply_socket_tuning (server, opt);

        const std::string endpoint = make_endpoint (opt.host, opt.port);
        if (zlink_bind (server, endpoint.c_str ()) != 0) {
            std::fprintf (stderr, "zlink_packet stream: bind failed: %s endpoint=%s\n",
                          zlink_strerror (zlink_errno ()), endpoint.c_str ());
            return 2;
        }

        g_server_instance = this;
        if (zlink_stream_packet_handler (server,
                                         &zlink_stream_packet_echo_server_t::on_packet_static, NULL)
            != 0) {
            std::fprintf (stderr, "zlink_packet stream: packet handler attach failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        std::signal (SIGINT, on_signal);
        std::signal (SIGTERM, on_signal);
        g_stop_flag = &stop;

        while (!stop.load (std::memory_order_acquire))
            std::this_thread::sleep_for (std::chrono::milliseconds (200));

        std::printf ("%s\n", stream_echo::make_metric_line (
                               "zlink_packet", opt.size, recv_msgs.load (std::memory_order_relaxed),
                               parse_error.load (std::memory_order_relaxed),
                               protocol_error.load (std::memory_order_relaxed),
                               send_error.load (std::memory_order_relaxed), 0)
                               .c_str ());
        return 0;
    }

  private:
    static void on_packet_static (void *stream_,
                                  const zlink_routing_id_t *rid_,
                                  zlink_msg_t *header_part_,
                                  zlink_msg_t *body_part_,
                                  void *userdata_)
    {
        (void) userdata_;
        zlink_stream_packet_echo_server_t *self = g_server_instance;
        if (!self || !stream_ || !rid_ || !header_part_ || !body_part_) {
            if (header_part_)
                (void) zlink_msg_close (header_part_);
            if (body_part_)
                (void) zlink_msg_close (body_part_);
            return;
        }

        self->on_packet (stream_, rid_, header_part_, body_part_);
    }

    void on_packet (void *stream_,
                    const zlink_routing_id_t *rid_,
                    zlink_msg_t *header_part_,
                    zlink_msg_t *body_part_)
    {
        zlink_msg_t reply;
        if (!build_packet_frame (&reply, header_part_, body_part_)) {
            send_error.fetch_add (1, std::memory_order_relaxed);
            (void) zlink_msg_close (header_part_);
            (void) zlink_msg_close (body_part_);
            return;
        }

        recv_msgs.fetch_add (1, std::memory_order_relaxed);
        if (zlink_send_part_rid (stream_, rid_, &reply, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL)
            != 0) {
            send_error.fetch_add (1, std::memory_order_relaxed);
        }

        (void) zlink_msg_close (&reply);
        (void) zlink_msg_close (header_part_);
        (void) zlink_msg_close (body_part_);
    }

    void cleanup ()
    {
        if (server) {
            zlink_close (server);
            server = NULL;
        }
        if (g_server_instance == this)
            g_server_instance = NULL;
        if (ctx) {
            zlink_ctx_term (ctx);
            ctx = NULL;
        }
    }

    server_options_t opt;
    void *ctx;
    void *server;

    std::atomic<long> recv_msgs;
    std::atomic<long> parse_error;
    std::atomic<long> protocol_error;
    std::atomic<long> send_error;

    std::atomic<bool> stop;
};

bool parse_options (int argc, char **argv, server_options_t &opt)
{
    const stream_echo::arg_reader_t args (argc, argv);

    opt.host = args.get_string ("--host", opt.host.c_str ());
    opt.port = args.get_int ("--port", opt.port, 1);
    opt.size = args.get_size ("--size", opt.size, k_min_payload_size);
    opt.sndbuf = args.get_int ("--sndbuf", opt.sndbuf, 1);
    opt.rcvbuf = args.get_int ("--rcvbuf", opt.rcvbuf, 1);
    opt.backlog = args.get_int ("--backlog", opt.backlog, 1);
    opt.tcp_nodelay = args.get_int ("--tcp-nodelay", opt.tcp_nodelay, 0);
    opt.io_threads = args.get_int ("--io-threads", opt.io_threads, 1);
    if (opt.size > k_max_payload_size) {
        std::fprintf (stderr, "zlink_packet stream: size too large %zu\n", opt.size);
        return false;
    }
    return true;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("test_scenario_stream_zlink_packet: no args -> skip\n");
        return 0;
    }

    server_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    zlink_stream_packet_echo_server_t server (opt);
    return server.run ();
}
