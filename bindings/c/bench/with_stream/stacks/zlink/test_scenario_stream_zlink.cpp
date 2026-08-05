#include "../common/stream_echo_common.hpp"

#include "../../../../../../core/include/zlink.h"

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <stdint.h>
#include <mutex>
#include <thread>

extern "C" {
typedef int (*zlink_stream_on_raw_fn) (const zlink_routing_id_t *, zlink_msg_t *);
int zlink_stream_attach_raw (void *s_, zlink_stream_on_raw_fn on_raw_);
}

namespace
{

static const size_t k_min_payload_size = 16;
static const size_t k_max_payload_size = 4 * 1024 * 1024;
static const unsigned char k_stream_event_connect = 0x01;
static const unsigned char k_stream_event_disconnect = 0x00;
static const size_t k_frame_shard_count = 64;

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
class zlink_stream_echo_server_t;
static zlink_stream_echo_server_t *g_server_instance = NULL;

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

uint32_t routing_id_key (const zlink_routing_id_t *rid_)
{
    return (static_cast<uint32_t> (rid_->data[0]) << 24)
           | (static_cast<uint32_t> (rid_->data[1]) << 16)
           | (static_cast<uint32_t> (rid_->data[2]) << 8) | static_cast<uint32_t> (rid_->data[3]);
}

size_t routing_id_shard (uint32_t rid_value_)
{
    return static_cast<size_t> (rid_value_ % k_frame_shard_count);
}

bool is_complete_stream_frame (const unsigned char *payload_, size_t payload_size_)
{
    if (!payload_ || payload_size_ < stream_echo::k_stream_packet_prefix_size)
        return false;

    const size_t header_size = static_cast<size_t> (stream_echo::load_u16_be (payload_));
    const size_t body_size = static_cast<size_t> (stream_echo::load_u32_be (payload_ + 2));
    if (!stream_echo::valid_frame_sizes (header_size, body_size))
        return false;

    const size_t frame_size = stream_echo::k_stream_packet_prefix_size + header_size + body_size;
    if (payload_size_ != frame_size)
        return false;

    return stream_echo::is_msg_name (payload_ + stream_echo::k_stream_packet_prefix_size,
                                     header_size);
}

bool try_parse_stream_frame (const unsigned char *payload_,
                             size_t payload_size_,
                             size_t *frame_size_out_)
{
    if (!payload_ || !frame_size_out_ || payload_size_ < stream_echo::k_stream_packet_prefix_size)
        return false;

    const size_t header_size = static_cast<size_t> (stream_echo::load_u16_be (payload_));
    const size_t body_size = static_cast<size_t> (stream_echo::load_u32_be (payload_ + 2));
    if (!stream_echo::valid_frame_sizes (header_size, body_size))
        return false;

    const size_t frame_size = stream_echo::k_stream_packet_prefix_size + header_size + body_size;
    if (payload_size_ < frame_size)
        return false;

    if (!stream_echo::is_msg_name (payload_ + stream_echo::k_stream_packet_prefix_size,
                                   header_size)) {
        return false;
    }

    *frame_size_out_ = frame_size;
    return true;
}

class zlink_stream_echo_server_t
{
  public:
    struct frame_shard_t
    {
        std::mutex mu;
        std::unordered_map<uint32_t, stream_echo::frame_buffer_t> buffers;
    };

    explicit zlink_stream_echo_server_t (const server_options_t &opt_) :
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

    ~zlink_stream_echo_server_t () { cleanup (); }

    int run ()
    {
        ctx = zlink_ctx_new ();
        if (!ctx) {
            std::fprintf (stderr, "zlink stream: zlink_ctx_new failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        (void) zlink_ctx_set (ctx, ZLINK_IO_THREADS, opt.io_threads);

        server = zlink_socket (ctx, ZLINK_SOCKET_STREAM);
        if (!server) {
            std::fprintf (stderr, "zlink stream: zlink_socket failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        apply_socket_tuning (server, opt);

        const std::string endpoint = make_endpoint (opt.host, opt.port);
        if (zlink_bind (server, endpoint.c_str ()) != 0) {
            std::fprintf (stderr, "zlink stream: bind failed: %s endpoint=%s\n",
                          zlink_strerror (zlink_errno ()), endpoint.c_str ());
            return 2;
        }

        g_server_instance = this;
        if (zlink_stream_attach_raw (server, &zlink_stream_echo_server_t::on_raw_packet_static)
            != 0) {
            std::fprintf (stderr, "zlink stream: dispatch attach failed: %s\n",
                          zlink_strerror (zlink_errno ()));
            return 2;
        }

        std::signal (SIGINT, on_signal);
        std::signal (SIGTERM, on_signal);
        g_stop_flag = &stop;

        while (!stop.load (std::memory_order_acquire))
            std::this_thread::sleep_for (std::chrono::milliseconds (200));

        std::printf ("%s\n", stream_echo::make_metric_line (
                               "zlink", opt.size, recv_msgs.load (std::memory_order_relaxed),
                               parse_error.load (std::memory_order_relaxed),
                               protocol_error.load (std::memory_order_relaxed),
                               send_error.load (std::memory_order_relaxed), 0)
                               .c_str ());
        return 0;
    }

  private:
    static int on_raw_packet_static (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
    {
        zlink_stream_echo_server_t *self = g_server_instance;
        if (!self || !rid_ || !msg_) {
            if (msg_)
                (void) zlink_msg_close (msg_);
            return 0;
        }

        return self->on_raw_packet (rid_, msg_);
    }

    void mark_parse_error ()
    {
        parse_error.fetch_add (1, std::memory_order_relaxed);
        protocol_error.fetch_add (1, std::memory_order_relaxed);
    }

    bool try_process_complete_chunk (const zlink_routing_id_t *rid_,
                                     zlink_msg_t *msg_,
                                     const unsigned char *payload_,
                                     size_t payload_size_)
    {
        if (!rid_ || !msg_ || !payload_ || payload_size_ == 0)
            return false;

        size_t offset = 0;
        size_t frame_count = 0;
        while (offset < payload_size_) {
            size_t frame_size = 0;
            if (!try_parse_stream_frame (payload_ + offset, payload_size_ - offset, &frame_size)) {
                if (offset == 0)
                    return false;
                mark_parse_error ();
                (void) zlink_msg_close (msg_);
                return true;
            }

            recv_msgs.fetch_add (1, std::memory_order_relaxed);
            if (offset == 0 && frame_size == payload_size_ && frame_count == 0) {
                if (zlink_send_part_rid (server, rid_, msg_, ZLINK_SEND_FLAGS_NONE,
                                         ZLINK_PART_FINAL)
                    != 0) {
                    send_error.fetch_add (1, std::memory_order_relaxed);
                    (void) zlink_msg_close (msg_);
                }
                return true;
            }

            zlink_msg_t reply;
            if (zlink_msg_init_size (&reply, frame_size) != 0) {
                send_error.fetch_add (1, std::memory_order_relaxed);
                (void) zlink_msg_close (msg_);
                return true;
            }
            std::memcpy (zlink_msg_data (&reply), payload_ + offset, frame_size);
            if (zlink_send_part_rid (server, rid_, &reply, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL)
                != 0) {
                send_error.fetch_add (1, std::memory_order_relaxed);
                (void) zlink_msg_close (&reply);
                (void) zlink_msg_close (msg_);
                return true;
            }
            (void) zlink_msg_close (&reply);
            offset += frame_size;
            ++frame_count;
        }

        (void) zlink_msg_close (msg_);
        return frame_count > 0;
    }

    int on_raw_packet (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
    {
        if (!rid_ || rid_->size != 4) {
            mark_parse_error ();
            (void) zlink_msg_close (msg_);
            return 0;
        }

        const unsigned char *payload = static_cast<const unsigned char *> (zlink_msg_data (msg_));
        const size_t payload_size = zlink_msg_size (msg_);
        if (payload_size == 0) {
            (void) zlink_msg_close (msg_);
            return 0;
        }

        if (payload_size == 1 && payload
            && (payload[0] == k_stream_event_connect || payload[0] == k_stream_event_disconnect)) {
            if (payload[0] == k_stream_event_disconnect) {
                const uint32_t rid_value = routing_id_key (rid_);
                frame_shard_t &shard = frame_shards[routing_id_shard (rid_value)];
                std::lock_guard<std::mutex> shard_lock (shard.mu);
                shard.buffers.erase (rid_value);
            }
            (void) zlink_msg_close (msg_);
            return 0;
        }

        const uint32_t rid_value = routing_id_key (rid_);
        frame_shard_t &shard = frame_shards[routing_id_shard (rid_value)];
        std::lock_guard<std::mutex> lock (shard.mu);
        stream_echo::frame_buffer_t &buffer = shard.buffers[rid_value];

        if (buffer.bytes.empty () && buffer.consumed == 0) {
            if (is_complete_stream_frame (payload, payload_size)) {
                recv_msgs.fetch_add (1, std::memory_order_relaxed);
                if (zlink_send_part_rid (server, rid_, msg_, ZLINK_SEND_FLAGS_NONE,
                                         ZLINK_PART_FINAL)
                    != 0) {
                    send_error.fetch_add (1, std::memory_order_relaxed);
                    (void) zlink_msg_close (msg_);
                }
                return 0;
            }
            if (try_process_complete_chunk (rid_, msg_, payload, payload_size))
                return 0;
        }

        stream_echo::append_frame_bytes (&buffer, payload, payload_size);
        (void) zlink_msg_close (msg_);

        if (stream_echo::has_invalid_declared_size (&buffer)) {
            mark_parse_error ();
            stream_echo::reset_frame_buffer (&buffer);
            return 0;
        }

        stream_echo::frame_view_t frame;
        while (stream_echo::try_peek_frame (&buffer, &frame)) {
            if (!stream_echo::is_msg_name (frame.header, frame.header_size)) {
                mark_parse_error ();
                stream_echo::reset_frame_buffer (&buffer);
                return 0;
            }

            zlink_msg_t reply;
            if (zlink_msg_init_size (&reply, frame.size) != 0) {
                send_error.fetch_add (1, std::memory_order_relaxed);
                stream_echo::reset_frame_buffer (&buffer);
                return 0;
            }
            std::memcpy (zlink_msg_data (&reply), frame.data, frame.size);
            recv_msgs.fetch_add (1, std::memory_order_relaxed);
            if (zlink_send_part_rid (server, rid_, &reply, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL)
                != 0) {
                send_error.fetch_add (1, std::memory_order_relaxed);
                (void) zlink_msg_close (&reply);
                stream_echo::reset_frame_buffer (&buffer);
                return 0;
            }
            (void) zlink_msg_close (&reply);
            stream_echo::consume_frame (&buffer, frame);
            stream_echo::compact_frame_buffer (&buffer);
        }
        return 0;
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
    std::array<frame_shard_t, k_frame_shard_count> frame_shards;
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
        std::fprintf (stderr, "zlink stream: size too large %zu\n", opt.size);
        return false;
    }
    return true;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("test_scenario_stream_zlink: no args -> skip\n");
        return 0;
    }

    server_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    zlink_stream_echo_server_t server (opt);
    return server.run ();
}
