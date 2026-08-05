#include "../common/stream_echo_common.hpp"

#include <zmq.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <map>
#include <string>

#ifndef ZMQ_STREAM
#define ZMQ_STREAM 11
#endif

namespace
{

static const size_t k_min_payload_size = 16;
static const size_t k_max_payload_size = 4 * 1024 * 1024;
static const unsigned char k_stream_event_connect = 0x01;
static const unsigned char k_stream_event_disconnect = 0x00;
static const int k_recv_batch_size = 256;

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
    (void) zmq_setsockopt (socket, ZMQ_SNDBUF, &opt.sndbuf, sizeof (opt.sndbuf));
    (void) zmq_setsockopt (socket, ZMQ_RCVBUF, &opt.rcvbuf, sizeof (opt.rcvbuf));
    (void) zmq_setsockopt (socket, ZMQ_BACKLOG, &opt.backlog, sizeof (opt.backlog));

#ifdef ZMQ_TCP_NODELAY
    (void) zmq_setsockopt (socket, ZMQ_TCP_NODELAY, &opt.tcp_nodelay, sizeof (opt.tcp_nodelay));
#endif

#ifdef ZMQ_STREAM_NOTIFY
    const int stream_notify = 1;
    (void) zmq_setsockopt (socket, ZMQ_STREAM_NOTIFY, &stream_notify, sizeof (stream_notify));
#endif

    const int hwm = 100;
    (void) zmq_setsockopt (socket, ZMQ_RCVHWM, &hwm, sizeof (hwm));
    (void) zmq_setsockopt (socket, ZMQ_SNDHWM, &hwm, sizeof (hwm));
}

bool is_stream_event_payload (const char *data, size_t size)
{
    return data && size == 1
           && (static_cast<unsigned char> (data[0]) == k_stream_event_connect
               || static_cast<unsigned char> (data[0]) == k_stream_event_disconnect);
}

int recv_one_stream_chunk_msg_nowait (void *server,
                                      zmq_msg_t *routing_id_out,
                                      zmq_msg_t *payload_out,
                                      std::atomic<long> &parse_error)
{
    if (!routing_id_out || !payload_out)
        return -1;

    while (true) {
        if (zmq_msg_init (routing_id_out) != 0)
            return -1;
        if (zmq_msg_init (payload_out) != 0) {
            zmq_msg_close (routing_id_out);
            return -1;
        }

        const int id_rc = zmq_msg_recv (routing_id_out, server, ZMQ_DONTWAIT);
        if (id_rc < 0) {
            const int err = zmq_errno ();
            zmq_msg_close (payload_out);
            zmq_msg_close (routing_id_out);
            if (err == EAGAIN || err == EINTR)
                return 0;
            parse_error.fetch_add (1, std::memory_order_relaxed);
            return -1;
        }

        const int data_rc = zmq_msg_recv (payload_out, server, ZMQ_DONTWAIT);
        if (data_rc < 0) {
            const int err = zmq_errno ();
            zmq_msg_close (payload_out);
            zmq_msg_close (routing_id_out);
            if (err != EAGAIN && err != EINTR)
                parse_error.fetch_add (1, std::memory_order_relaxed);
            return -1;
        }

        const char *id_data = static_cast<const char *> (zmq_msg_data (routing_id_out));
        const size_t id_size = zmq_msg_size (routing_id_out);
        const char *payload_data = static_cast<const char *> (zmq_msg_data (payload_out));
        const size_t payload_size = zmq_msg_size (payload_out);

        if (!id_data || id_size == 0) {
            parse_error.fetch_add (1, std::memory_order_relaxed);
            zmq_msg_close (payload_out);
            zmq_msg_close (routing_id_out);
            continue;
        }

        if (payload_size == 0 || is_stream_event_payload (payload_data, payload_size)) {
            zmq_msg_close (payload_out);
            zmq_msg_close (routing_id_out);
            continue;
        }

        return 1;
    }
}

bool send_stream_reply_msg (void *server,
                            zmq_msg_t *routing_id_msg,
                            const unsigned char *payload_data,
                            size_t payload_size)
{
    if (!server || !routing_id_msg || !payload_data)
        return false;

    if (zmq_msg_send (routing_id_msg, server, ZMQ_SNDMORE) < 0)
        return false;
    return zmq_send (server, payload_data, payload_size, 0) == static_cast<int> (payload_size);
}

class zmq_stream_echo_server_t
{
  public:
    explicit zmq_stream_echo_server_t (const server_options_t &opt_) :
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

    ~zmq_stream_echo_server_t () { cleanup (); }

    int run ()
    {
        ctx = zmq_ctx_new ();
        if (!ctx) {
            std::fprintf (stderr, "zmq stream: zmq_ctx_new failed: %s\n",
                          zmq_strerror (zmq_errno ()));
            return 2;
        }

        (void) zmq_ctx_set (ctx, ZMQ_IO_THREADS, std::max (1, opt.io_threads));

        server = zmq_socket (ctx, ZMQ_STREAM);
        if (!server) {
            std::fprintf (stderr, "zmq stream: zmq_socket failed: %s\n",
                          zmq_strerror (zmq_errno ()));
            return 2;
        }

        const int linger_ms = 0;
        (void) zmq_setsockopt (server, ZMQ_LINGER, &linger_ms, sizeof (linger_ms));
        apply_socket_tuning (server, opt);

        const std::string endpoint = make_endpoint (opt.host, opt.port);
        if (zmq_bind (server, endpoint.c_str ()) != 0) {
            std::fprintf (stderr, "zmq stream: bind failed: %s endpoint=%s\n",
                          zmq_strerror (zmq_errno ()), endpoint.c_str ());
            return 2;
        }

        std::signal (SIGINT, on_signal);
        std::signal (SIGTERM, on_signal);
        g_stop_flag = &stop;

        while (!stop.load (std::memory_order_acquire)) {
            zmq_pollitem_t item[] = {{server, 0, ZMQ_POLLIN, 0}};
            const int prc = zmq_poll (item, 1, 1000);
            if (prc < 0) {
                if (zmq_errno () == EINTR)
                    continue;
                parse_error.fetch_add (1, std::memory_order_relaxed);
                break;
            }
            if (prc == 0 || (item[0].revents & ZMQ_POLLIN) == 0)
                continue;

            for (;;) {
                const int processed = recv_and_process_batch ();
                if (processed < 0 || processed < k_recv_batch_size)
                    break;
            }
        }

        std::printf ("%s\n", stream_echo::make_metric_line (
                               "zmq", opt.size, recv_msgs.load (std::memory_order_relaxed),
                               parse_error.load (std::memory_order_relaxed),
                               protocol_error.load (std::memory_order_relaxed),
                               send_error.load (std::memory_order_relaxed), 0)
                               .c_str ());

        return 0;
    }

  private:
    int recv_and_process_batch ()
    {
        int processed = 0;
        for (; processed < k_recv_batch_size; ++processed) {
            zmq_msg_t routing_id_msg;
            zmq_msg_t payload_msg;
            const int rc =
              recv_one_stream_chunk_msg_nowait (server, &routing_id_msg, &payload_msg, parse_error);
            if (rc == 0)
                break;
            if (rc < 0)
                return processed == 0 ? -1 : processed;

            const char *rid_data = static_cast<const char *> (zmq_msg_data (&routing_id_msg));
            const size_t rid_size = zmq_msg_size (&routing_id_msg);
            const unsigned char *payload_data =
              static_cast<const unsigned char *> (zmq_msg_data (&payload_msg));
            const size_t payload_size = zmq_msg_size (&payload_msg);

            stream_echo::frame_buffer_t &buffer =
              frame_buffers[std::string (rid_data, rid_data + rid_size)];
            stream_echo::append_frame_bytes (&buffer, payload_data, payload_size);
            if (stream_echo::has_invalid_declared_size (&buffer)) {
                parse_error.fetch_add (1, std::memory_order_relaxed);
                protocol_error.fetch_add (1, std::memory_order_relaxed);
                stream_echo::reset_frame_buffer (&buffer);
                zmq_msg_close (&payload_msg);
                zmq_msg_close (&routing_id_msg);
                continue;
            }

            stream_echo::frame_view_t frame;
            while (stream_echo::try_peek_frame (&buffer, &frame)) {
                if (!stream_echo::is_msg_name (frame.header, frame.header_size)) {
                    parse_error.fetch_add (1, std::memory_order_relaxed);
                    protocol_error.fetch_add (1, std::memory_order_relaxed);
                    stream_echo::reset_frame_buffer (&buffer);
                    break;
                }
                recv_msgs.fetch_add (1, std::memory_order_relaxed);
                if (!send_stream_reply_msg (server, &routing_id_msg, frame.data, frame.size)) {
                    send_error.fetch_add (1, std::memory_order_relaxed);
                    break;
                }
                stream_echo::consume_frame (&buffer, frame);
                stream_echo::compact_frame_buffer (&buffer);
            }

            zmq_msg_close (&payload_msg);
            zmq_msg_close (&routing_id_msg);
        }
        return processed;
    }

    void cleanup ()
    {
        if (server) {
            zmq_close (server);
            server = NULL;
        }

        if (ctx) {
            zmq_ctx_term (ctx);
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
    std::map<std::string, stream_echo::frame_buffer_t> frame_buffers;
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
        std::fprintf (stderr, "zmq stream: size too large %zu\n", opt.size);
        return false;
    }

    return true;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("test_scenario_stream_zmq: no args -> skip\n");
        return 0;
    }

    server_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    zmq_stream_echo_server_t server (opt);
    return server.run ();
}
