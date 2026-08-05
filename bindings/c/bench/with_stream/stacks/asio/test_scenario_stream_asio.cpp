#include "../common/stream_echo_common.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using boost::asio::ip::tcp;

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
        io_threads (8)
    {
    }
};

class asio_echo_server_t;

class asio_session_t : public std::enable_shared_from_this<asio_session_t>
{
  public:
    asio_session_t (asio_echo_server_t &owner_, boost::asio::io_context &io_);

    tcp::socket &socket_ref () { return socket; }
    void start ();
    void close ();

  private:
    void start_read_chunk ();
    void on_read_chunk (const boost::system::error_code &ec, size_t bytes);
    void start_write_chunk ();
    void on_write (const boost::system::error_code &ec, size_t bytes);
    void close_internal ();

    asio_echo_server_t &owner;
    tcp::socket socket;
    boost::asio::strand<boost::asio::io_context::executor_type> strand;

    bool closed;
    std::array<unsigned char, 16384> read_chunk;
    std::vector<unsigned char> pending_write;
    stream_echo::frame_buffer_t frame_buffer;
};

class asio_echo_server_t
{
  public:
    explicit asio_echo_server_t (const server_options_t &opt_) :
        opt (opt_),
        io (),
        work_guard (boost::asio::make_work_guard (io)),
        acceptor (io),
        signals (io, SIGINT, SIGTERM),
        active_connections (0),
        recv_msgs (0),
        parse_error (0),
        protocol_error (0),
        send_error (0),
        stopping (false)
    {
    }

    int run ()
    {
        boost::system::error_code ec;
        const boost::asio::ip::address addr = boost::asio::ip::make_address (opt.host, ec);
        if (ec) {
            std::fprintf (stderr, "asio server: invalid host %s\n", opt.host.c_str ());
            return 2;
        }

        const tcp::endpoint ep (addr, static_cast<unsigned short> (opt.port));
        acceptor.open (ep.protocol (), ec);
        if (ec) {
            std::fprintf (stderr, "asio server: acceptor.open failed: %s\n",
                          ec.message ().c_str ());
            return 2;
        }

        acceptor.set_option (tcp::acceptor::reuse_address (true), ec);
        acceptor.bind (ep, ec);
        if (ec) {
            std::fprintf (stderr, "asio server: bind failed: %s\n", ec.message ().c_str ());
            return 2;
        }

        acceptor.listen (opt.backlog, ec);
        if (ec) {
            std::fprintf (stderr, "asio server: listen failed: %s\n", ec.message ().c_str ());
            return 2;
        }

        signals.async_wait ([this] (const boost::system::error_code &, int) { stop (); });

        start_accept ();

        const int worker_count = std::max (1, opt.io_threads);
        for (int i = 0; i < worker_count; ++i)
            workers.push_back (std::thread ([this] () { io.run (); }));

        for (size_t i = 0; i < workers.size (); ++i) {
            if (workers[i].joinable ())
                workers[i].join ();
        }

        std::printf ("%s\n", stream_echo::make_metric_line (
                               "asio", opt.size, recv_msgs.load (std::memory_order_relaxed),
                               parse_error.load (std::memory_order_relaxed),
                               protocol_error.load (std::memory_order_relaxed),
                               send_error.load (std::memory_order_relaxed),
                               active_connections.load (std::memory_order_relaxed))
                               .c_str ());

        return 0;
    }

    void stop ()
    {
        if (stopping.exchange (true, std::memory_order_acq_rel))
            return;

        boost::system::error_code ec;
        acceptor.cancel (ec);
        acceptor.close (ec);
        work_guard.reset ();
        io.stop ();
    }

    void on_session_open () { active_connections.fetch_add (1, std::memory_order_relaxed); }

    void on_session_close ()
    {
        if (active_connections.load (std::memory_order_relaxed) > 0)
            active_connections.fetch_sub (1, std::memory_order_relaxed);
    }

    void on_recv_frame (size_t payload_size)
    {
        if (payload_size > 0)
            recv_msgs.fetch_add (1, std::memory_order_relaxed);
    }

    void on_send_error () { send_error.fetch_add (1, std::memory_order_relaxed); }

    void apply_socket_tuning (tcp::socket &socket)
    {
        boost::system::error_code ec;
        socket.set_option (boost::asio::ip::tcp::no_delay (opt.tcp_nodelay != 0), ec);
        boost::asio::socket_base::send_buffer_size snd (opt.sndbuf);
        socket.set_option (snd, ec);
        boost::asio::socket_base::receive_buffer_size rcv (opt.rcvbuf);
        socket.set_option (rcv, ec);
    }

  private:
    void start_accept ()
    {
        if (stopping.load (std::memory_order_acquire))
            return;

        std::shared_ptr<asio_session_t> session = std::make_shared<asio_session_t> (*this, io);
        acceptor.async_accept (session->socket_ref (),
                               [this, session] (const boost::system::error_code &ec) {
                                   if (!ec) {
                                       apply_socket_tuning (session->socket_ref ());
                                       session->start ();
                                   }
                                   if (!stopping.load (std::memory_order_acquire))
                                       start_accept ();
                               });
    }

    server_options_t opt;
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
    tcp::acceptor acceptor;
    boost::asio::signal_set signals;
    std::vector<std::thread> workers;

    std::atomic<long> active_connections;
    std::atomic<long> recv_msgs;
    std::atomic<long> parse_error;
    std::atomic<long> protocol_error;
    std::atomic<long> send_error;
    std::atomic<bool> stopping;
};

asio_session_t::asio_session_t (asio_echo_server_t &owner_, boost::asio::io_context &io_) :
    owner (owner_),
    socket (io_),
    strand (boost::asio::make_strand (io_)),
    closed (false),
    read_chunk (),
    pending_write (),
    frame_buffer ()
{
}

void asio_session_t::start ()
{
    owner.on_session_open ();
    start_read_chunk ();
}

void asio_session_t::start_read_chunk ()
{
    if (closed)
        return;

    const std::shared_ptr<asio_session_t> self = shared_from_this ();
    socket.async_read_some (boost::asio::buffer (read_chunk),
                            boost::asio::bind_executor (
                              strand, [self] (const boost::system::error_code &ec, size_t bytes) {
                                  self->on_read_chunk (ec, bytes);
                              }));
}

void asio_session_t::on_read_chunk (const boost::system::error_code &ec, size_t bytes)
{
    if (closed)
        return;

    if (ec || bytes == 0) {
        close_internal ();
        return;
    }

    stream_echo::append_frame_bytes (&frame_buffer, &read_chunk[0], bytes);
    if (stream_echo::has_invalid_declared_size (&frame_buffer)) {
        close_internal ();
        return;
    }

    pending_write.clear ();
    stream_echo::frame_view_t frame;
    while (stream_echo::try_peek_frame (&frame_buffer, &frame)) {
        if (!stream_echo::is_msg_name (frame.header, frame.header_size)) {
            close_internal ();
            return;
        }
        const size_t old_size = pending_write.size ();
        pending_write.resize (old_size + frame.size);
        std::memcpy (&pending_write[old_size], frame.data, frame.size);
        owner.on_recv_frame (frame.payload_size);
        stream_echo::consume_frame (&frame_buffer, frame);
        stream_echo::compact_frame_buffer (&frame_buffer);
    }

    if (pending_write.empty ()) {
        start_read_chunk ();
        return;
    }
    start_write_chunk ();
}

void asio_session_t::start_write_chunk ()
{
    if (closed || pending_write.empty ())
        return;

    const std::shared_ptr<asio_session_t> self = shared_from_this ();
    boost::asio::async_write (
      socket, boost::asio::buffer (pending_write),
      boost::asio::bind_executor (strand, [self] (const boost::system::error_code &ec,
                                                  size_t bytes) { self->on_write (ec, bytes); }));
}

void asio_session_t::on_write (const boost::system::error_code &ec, size_t bytes)
{
    if (closed)
        return;

    if (ec || bytes != pending_write.size ()) {
        owner.on_send_error ();
        close_internal ();
        return;
    }

    pending_write.clear ();
    start_read_chunk ();
}

void asio_session_t::close ()
{
    const std::shared_ptr<asio_session_t> self = shared_from_this ();
    boost::asio::post (strand, [self] () { self->close_internal (); });
}

void asio_session_t::close_internal ()
{
    if (closed)
        return;
    closed = true;
    pending_write.clear ();

    boost::system::error_code ec;
    socket.cancel (ec);
    socket.shutdown (tcp::socket::shutdown_both, ec);
    socket.close (ec);

    owner.on_session_close ();
}

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
        std::fprintf (stderr, "asio server: size too large %zu\n", opt.size);
        return false;
    }

    return true;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("test_scenario_stream_asio: no args -> skip\n");
        return 0;
    }

    server_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    asio_echo_server_t server (opt);
    return server.run ();
}
