#include "../common/stream_echo_common.hpp"

#include "server/asio/service.h"
#include "server/asio/tcp_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace CppServer::Asio;

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
        port (38003),
        size (1024),
        sndbuf (1024 * 1024),
        rcvbuf (1024 * 1024),
        backlog (32768),
        tcp_nodelay (1),
        io_threads (8)
    {
    }
};

struct metrics_t
{
    std::atomic<long> recv_msgs;
    std::atomic<long> parse_error;
    std::atomic<long> protocol_error;
    std::atomic<long> send_error;
    std::atomic<long> active_connections;

    metrics_t () :
        recv_msgs (0), parse_error (0), protocol_error (0), send_error (0), active_connections (0)
    {
    }
};

static std::atomic<bool> *g_stop_flag = NULL;

void on_signal (int)
{
    if (g_stop_flag)
        g_stop_flag->store (true, std::memory_order_release);
}

class stream_echo_session_t : public TCPSession
{
  public:
    stream_echo_session_t (const std::shared_ptr<TCPServer> &server,
                           const server_options_t &opt_,
                           metrics_t &metrics_) :
        TCPSession (server), opt (opt_), metrics (metrics_), frame_buffer (), pending_send ()
    {
    }

  protected:
    void onConnected () override
    {
        metrics.active_connections.fetch_add (1, std::memory_order_relaxed);
        SetupReceiveBufferSize (static_cast<size_t> (std::max (1, opt.rcvbuf)));
        SetupSendBufferSize (static_cast<size_t> (std::max (1, opt.sndbuf)));
        SetupReceiveBufferLimit (k_max_payload_size * 4);
        SetupSendBufferLimit (k_max_payload_size * 4);
    }

    void onDisconnected () override
    {
        if (metrics.active_connections.load (std::memory_order_relaxed) > 0)
            metrics.active_connections.fetch_sub (1, std::memory_order_relaxed);
    }

    void onReceived (const void *buffer, size_t size) override
    {
        if (!buffer || size == 0)
            return;

        stream_echo::append_frame_bytes (&frame_buffer, static_cast<const unsigned char *> (buffer),
                                         size);
        if (stream_echo::has_invalid_declared_size (&frame_buffer)) {
            metrics.parse_error.fetch_add (1, std::memory_order_relaxed);
            metrics.protocol_error.fetch_add (1, std::memory_order_relaxed);
            stream_echo::reset_frame_buffer (&frame_buffer);
            Disconnect ();
            return;
        }

        stream_echo::frame_view_t frame;
        while (stream_echo::try_peek_frame (&frame_buffer, &frame)) {
            if (!stream_echo::is_msg_name (frame.header, frame.header_size)) {
                metrics.parse_error.fetch_add (1, std::memory_order_relaxed);
                metrics.protocol_error.fetch_add (1, std::memory_order_relaxed);
                stream_echo::reset_frame_buffer (&frame_buffer);
                Disconnect ();
                return;
            }

            pending_send.resize (frame.size);
            std::memcpy (&pending_send[0], frame.data, frame.size);
            if (!SendAsync (&pending_send[0], pending_send.size ())) {
                metrics.send_error.fetch_add (1, std::memory_order_relaxed);
                Disconnect ();
                return;
            }

            metrics.recv_msgs.fetch_add (1, std::memory_order_relaxed);
            stream_echo::consume_frame (&frame_buffer, frame);
            stream_echo::compact_frame_buffer (&frame_buffer);
        }
    }

    void onError (int, const std::string &, const std::string &) override
    {
        metrics.send_error.fetch_add (1, std::memory_order_relaxed);
    }

  private:
    const server_options_t &opt;
    metrics_t &metrics;
    stream_echo::frame_buffer_t frame_buffer;
    std::vector<unsigned char> pending_send;
};

class stream_echo_server_t : public TCPServer
{
  public:
    stream_echo_server_t (const std::shared_ptr<Service> &service,
                          const server_options_t &opt_,
                          metrics_t &metrics_) :
        TCPServer (service, opt_.host, opt_.port), opt (opt_), metrics (metrics_)
    {
    }

  protected:
    std::shared_ptr<TCPSession> CreateSession (const std::shared_ptr<TCPServer> &server) override
    {
        return std::make_shared<stream_echo_session_t> (server, opt, metrics);
    }

    void onStarted () override
    {
        std::error_code ec;
        acceptor ().listen (opt.backlog, ec);
        if (ec) {
            std::fprintf (stderr, "cppserver stream: listen(backlog=%d) failed: %s\n", opt.backlog,
                          ec.message ().c_str ());
        }
    }

    void onError (int error, const std::string &category, const std::string &message) override
    {
        std::fprintf (stderr, "cppserver stream error code=%d category=%s message=%s\n", error,
                      category.c_str (), message.c_str ());
    }

  private:
    server_options_t opt;
    metrics_t &metrics;
};

bool parse_options (int argc, char **argv, server_options_t &opt)
{
    stream_echo::arg_reader_t args (argc, argv);

    opt.host = args.get_string ("--host", opt.host.c_str ());
    opt.port = args.get_int ("--port", opt.port, 1);
    opt.size = args.get_size ("--size", opt.size, k_min_payload_size);
    opt.sndbuf = args.get_int ("--sndbuf", opt.sndbuf, 1);
    opt.rcvbuf = args.get_int ("--rcvbuf", opt.rcvbuf, 1);
    opt.backlog = args.get_int ("--backlog", opt.backlog, 1);
    opt.tcp_nodelay = args.get_int ("--tcp-nodelay", opt.tcp_nodelay, 0);
    opt.io_threads = args.get_int ("--io-threads", opt.io_threads, 1);
    if (opt.size > k_max_payload_size) {
        std::fprintf (stderr, "cppserver stream: size too large %zu\n", opt.size);
        return false;
    }

    return true;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("stream_fixed_server: no args -> skip\n");
        return 0;
    }

    server_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    std::atomic<bool> stop (false);
    g_stop_flag = &stop;
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    metrics_t metrics;

    auto service = std::make_shared<Service> (std::max (1, opt.io_threads));
    auto server = std::make_shared<stream_echo_server_t> (service, opt, metrics);

    server->SetupNoDelay (opt.tcp_nodelay != 0);
    server->SetupReuseAddress (true);
    server->SetupReusePort (true);

    service->Start ();
    if (!server->Start ()) {
        std::fprintf (stderr, "cppserver stream: failed to start server\n");
        service->Stop ();
        return 2;
    }

    while (!stop.load (std::memory_order_acquire))
        std::this_thread::sleep_for (std::chrono::milliseconds (200));

    server->Stop ();
    service->Stop ();

    std::printf ("%s\n",
                 stream_echo::make_metric_line (
                   "cppserver", opt.size, metrics.recv_msgs.load (std::memory_order_relaxed),
                   metrics.parse_error.load (std::memory_order_relaxed),
                   metrics.protocol_error.load (std::memory_order_relaxed),
                   metrics.send_error.load (std::memory_order_relaxed),
                   metrics.active_connections.load (std::memory_order_relaxed))
                   .c_str ());

    return 0;
}
