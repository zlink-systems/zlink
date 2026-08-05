#include "stream_client_core.hpp"

#include "../common/stream_echo_common.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace stream_client
{
namespace
{

using boost::asio::ip::tcp;

static const int k_connect_batch = 1024;
static const int k_rtt_sample_rate = 16;
static const size_t k_rtt_sample_capacity = 1024 * 1024;
static const size_t k_min_payload_size = 16;
static const size_t k_max_payload_size = 4 * 1024 * 1024;

struct client_options_t
{
    std::string host;
    int port;
    int ccu;
    size_t size;
    int duration;
    int inflight;
    int io_threads;

    client_options_t () :
        host ("127.0.0.1"),
        port (38001),
        ccu (1000),
        size (1024),
        duration (5),
        inflight (10),
        io_threads (1)
    {
    }
};

static const int k_connect_timeout_s = 60;
static const int k_connect_retry_delay_ms = 25;
static const int k_socket_sndbuf = 256 * 1024;
static const int k_socket_rcvbuf = 256 * 1024;
static const int k_socket_tcp_nodelay = 1;
static const size_t k_loopback_port_headroom = 64;

class echo_client_t;

class client_session_t : public std::enable_shared_from_this<client_session_t>
{
  public:
    client_session_t (echo_client_t &owner_,
                      boost::asio::io_context &io_,
                      const tcp::endpoint &source_bind_ep_);

    void begin_connect (const tcp::endpoint &endpoint_);
    void start_traffic ();
    void request_close ();
    bool connected () const;

  private:
    void do_connect ();
    void on_connect (const boost::system::error_code &ec);
    void start_read_header ();
    void on_read_header (const boost::system::error_code &ec, size_t bytes);
    void start_read_body (size_t len);
    void on_read_body (const boost::system::error_code &ec, size_t bytes);
    void maybe_send_more ();
    void send_one ();
    void on_write (const boost::system::error_code &ec, size_t bytes);
    void close_internal ();
    void apply_socket_tuning ();

    echo_client_t &owner;
    tcp::socket socket;
    boost::asio::steady_timer retry_timer;
    boost::asio::strand<boost::asio::io_context::executor_type> strand;

    bool connect_reported;
    bool connected_flag;
    bool closed;
    bool write_inflight;
    bool connect_started;
    size_t outstanding;
    std::chrono::steady_clock::time_point connect_deadline;

    tcp::endpoint endpoint;
    tcp::endpoint source_bind_ep;
    std::vector<unsigned char> read_header;
    std::vector<unsigned char> read_body;
    std::vector<unsigned char> write_buf;
    size_t read_header_size;
};

struct loopback_bind_plan_t
{
    size_t port_capacity;
    std::vector<boost::asio::ip::address_v4> source_addrs;

    loopback_bind_plan_t () : port_capacity (0), source_addrs () {}
};

size_t read_ipv4_ephemeral_port_capacity ()
{
    std::ifstream in ("/proc/sys/net/ipv4/ip_local_port_range");
    if (!in.is_open ())
        return 0;

    int low = 0;
    int high = 0;
    in >> low >> high;
    if (!in.good () && !in.eof ())
        return 0;
    if (low <= 0 || high <= 0 || high < low)
        return 0;
    return static_cast<size_t> (high - low + 1);
}

boost::asio::ip::address_v4 make_loopback_shard_addr (size_t idx)
{
    const size_t block = idx / 254;
    const size_t tail = (idx % 254) + 1;
    boost::asio::ip::address_v4::bytes_type bytes;
    bytes[0] = 127;
    bytes[1] = 0;
    bytes[2] = static_cast<unsigned char> (block & 0xFF);
    bytes[3] = static_cast<unsigned char> (tail);
    return boost::asio::ip::address_v4 (bytes);
}

loopback_bind_plan_t make_loopback_bind_plan (const tcp::endpoint &endpoint, int ccu)
{
    loopback_bind_plan_t plan;
    if (ccu <= 0)
        return plan;
    if (!endpoint.address ().is_v4 ())
        return plan;
    if (!endpoint.address ().to_v4 ().is_loopback ())
        return plan;

    plan.port_capacity = read_ipv4_ephemeral_port_capacity ();
    if (plan.port_capacity == 0)
        return plan;

    const size_t usable_ports = plan.port_capacity > k_loopback_port_headroom
                                  ? plan.port_capacity - k_loopback_port_headroom
                                  : plan.port_capacity;
    if (usable_ports == 0)
        return plan;

    const size_t required =
      static_cast<size_t> (ccu + static_cast<int> (usable_ports) - 1) / usable_ports;
    if (required <= 1)
        return plan;

    plan.source_addrs.reserve (required);
    for (size_t i = 0; i < required; ++i)
        plan.source_addrs.push_back (make_loopback_shard_addr (i));
    return plan;
}

class echo_client_t
{
  public:
    explicit echo_client_t (const client_options_t &opt_) :
        opt (opt_),
        io (),
        work_guard (boost::asio::make_work_guard (io)),
        next_connect_idx (0),
        connect_inflight (0),
        connect_success (0),
        connect_fail (0),
        connect_completed (0),
        sent_msgs (0),
        recv_msgs (0),
        parse_error (0),
        timeout_error (0),
        send_error (0),
        outstanding_total (0),
        abandoned_outstanding (0),
        seq_gen (0),
        measuring (false),
        sample_overwrite_idx (0),
        endpoint (boost::asio::ip::make_address (opt.host), static_cast<unsigned short> (opt.port))
    {
        connected_sessions.reserve (static_cast<size_t> (std::max (1, opt.ccu)));
        rtt_samples_us.assign (k_rtt_sample_capacity, 0.0);
        loopback_bind_plan = make_loopback_bind_plan (endpoint, opt.ccu);
        if (!loopback_bind_plan.source_addrs.empty ()) {
            std::fprintf (stderr,
                          "stream client: loopback source-ip sharding enabled "
                          "(ccu=%d, port_capacity=%zu, shards=%zu)\n",
                          opt.ccu, loopback_bind_plan.port_capacity,
                          loopback_bind_plan.source_addrs.size ());
        }
    }

    int run ()
    {
        const int worker_count = std::max (1, opt.io_threads);
        for (int i = 0; i < worker_count; ++i) {
            workers.push_back (std::thread ([this] () { io.run (); }));
        }

        sessions.reserve (static_cast<size_t> (std::max (1, opt.ccu)));
        for (int i = 0; i < opt.ccu; ++i) {
            sessions.push_back (std::make_shared<client_session_t> (
              *this, io, source_bind_endpoint_for (static_cast<size_t> (i))));
        }

        const std::chrono::steady_clock::time_point connect_begin =
          std::chrono::steady_clock::now ();
        schedule_connects ();

        const auto connect_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (k_connect_timeout_s);
        {
            std::unique_lock<std::mutex> lk (connect_mu);
            while (connect_completed.load (std::memory_order_acquire)
                   < static_cast<long> (sessions.size ())) {
                if (connect_cv.wait_until (lk, connect_deadline) == std::cv_status::timeout)
                    break;
            }
        }
        const std::chrono::steady_clock::time_point connect_done =
          std::chrono::steady_clock::now ();
        const double connect_ms = static_cast<double> (
          std::chrono::duration_cast<std::chrono::milliseconds> (connect_done - connect_begin)
            .count ());

        if (connect_completed.load (std::memory_order_acquire)
            < static_cast<long> (sessions.size ())) {
            const long unresolved = static_cast<long> (sessions.size ())
                                    - connect_completed.load (std::memory_order_acquire);
            timeout_error.fetch_add (unresolved, std::memory_order_relaxed);
        }

        if (connect_success.load (std::memory_order_relaxed) == static_cast<long> (sessions.size ())
            && connect_fail.load (std::memory_order_relaxed) == 0
            && timeout_error.load (std::memory_order_relaxed) == 0) {
            measure_start = std::chrono::steady_clock::now ();
            measure_end = measure_start + std::chrono::seconds (std::max (1, opt.duration));
            measuring.store (true, std::memory_order_release);
            std::vector<std::shared_ptr<client_session_t>> ready_sessions;
            {
                std::lock_guard<std::mutex> lk (connected_mu);
                ready_sessions = connected_sessions;
            }
            for (size_t i = 0; i < ready_sessions.size (); ++i)
                ready_sessions[i]->start_traffic ();

            std::this_thread::sleep_until (measure_end);
            measuring.store (false, std::memory_order_release);
        }

        const auto drain_deadline = std::chrono::steady_clock::now ()
                                    + std::chrono::seconds (std::max (5, k_connect_timeout_s));
        while (std::chrono::steady_clock::now () < drain_deadline) {
            if (outstanding_total.load (std::memory_order_relaxed) <= 0)
                break;
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        for (size_t i = 0; i < sessions.size (); ++i)
            sessions[i]->request_close ();

        work_guard.reset ();
        io.stop ();
        for (size_t i = 0; i < workers.size (); ++i) {
            if (workers[i].joinable ())
                workers[i].join ();
        }

        const long sent = sent_msgs.load (std::memory_order_relaxed);
        const long recv = recv_msgs.load (std::memory_order_relaxed);
        const long incomplete = sent > recv ? sent - recv : 0;
        const double incomplete_ratio =
          sent > 0 ? static_cast<double> (incomplete) / static_cast<double> (sent) : 0.0;

        const double duration_s = static_cast<double> (std::max (1, opt.duration));
        const double throughput_msg_s =
          duration_s > 0.0 ? static_cast<double> (recv) / duration_s : 0.0;
        const double throughput_mib_s =
          duration_s > 0.0 ? (static_cast<double> (recv) * static_cast<double> (opt.size * 2))
                               / duration_s / (1024.0 * 1024.0)
                           : 0.0;

        stream_echo::summary_stats_t stats;
        const size_t sample_count = std::min<size_t> (
          sample_overwrite_idx.load (std::memory_order_relaxed), rtt_samples_us.size ());
        if (sample_count > 0) {
            std::vector<double> snapshot;
            snapshot.reserve (sample_count);
            for (size_t i = 0; i < sample_count; ++i)
                snapshot.push_back (rtt_samples_us[i]);
            stats = stream_echo::make_summary_stats (snapshot);
        }

        const bool have_samples = stats.p95_us > 0.0;
        const bool pass = connect_fail.load (std::memory_order_relaxed) == 0
                          && parse_error.load (std::memory_order_relaxed) == 0
                          && timeout_error.load (std::memory_order_relaxed) == 0
                          && incomplete_ratio <= 0.01 && have_samples;

        std::printf (
          "%s\n",
          stream_echo::make_result_line (throughput_msg_s, throughput_mib_s, stats, pass).c_str ());

        (void) connect_ms;

        return pass ? 0 : 2;
    }

    void schedule_connects ()
    {
        std::lock_guard<std::mutex> lk (connect_sched_mu);
        while (connect_inflight.load (std::memory_order_relaxed) < k_connect_batch) {
            const size_t idx = next_connect_idx.fetch_add (1, std::memory_order_relaxed);
            if (idx >= sessions.size ())
                break;
            connect_inflight.fetch_add (1, std::memory_order_relaxed);
            sessions[idx]->begin_connect (endpoint);
        }
    }

    void on_connect_result (bool success, const std::shared_ptr<client_session_t> &session)
    {
        if (success) {
            connect_success.fetch_add (1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk (connected_mu);
            connected_sessions.push_back (session);
        } else {
            connect_fail.fetch_add (1, std::memory_order_relaxed);
        }
        connect_completed.fetch_add (1, std::memory_order_relaxed);
        connect_inflight.fetch_sub (1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk (connect_mu);
            connect_cv.notify_all ();
        }

        schedule_connects ();
    }

    bool allow_new_send () const
    {
        return measuring.load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < measure_end;
    }

    uint64_t next_seq () { return seq_gen.fetch_add (1, std::memory_order_relaxed) + 1; }

    void mark_send_success ()
    {
        sent_msgs.fetch_add (1, std::memory_order_relaxed);
        outstanding_total.fetch_add (1, std::memory_order_relaxed);
    }

    void mark_recv_success ()
    {
        recv_msgs.fetch_add (1, std::memory_order_relaxed);
        outstanding_total.fetch_sub (1, std::memory_order_relaxed);
    }

    void mark_abandoned (size_t n)
    {
        if (n == 0)
            return;
        abandoned_outstanding.fetch_add (static_cast<long> (n), std::memory_order_relaxed);
        outstanding_total.fetch_sub (static_cast<long> (n), std::memory_order_relaxed);
    }

    void add_parse_error () { parse_error.fetch_add (1, std::memory_order_relaxed); }

    void add_send_error () { send_error.fetch_add (1, std::memory_order_relaxed); }

    void add_rtt_sample (double us)
    {
        if (rtt_samples_us.empty ())
            return;

        const size_t idx =
          sample_overwrite_idx.fetch_add (1, std::memory_order_relaxed) % rtt_samples_us.size ();
        rtt_samples_us[idx] = us;
    }

    const client_options_t &options () const { return opt; }

  private:
    tcp::endpoint source_bind_endpoint_for (size_t idx) const
    {
        if (loopback_bind_plan.source_addrs.empty ())
            return tcp::endpoint ();
        const boost::asio::ip::address_v4 &addr =
          loopback_bind_plan.source_addrs[idx % loopback_bind_plan.source_addrs.size ()];
        return tcp::endpoint (addr, 0);
    }

    client_options_t opt;
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;

    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<client_session_t>> sessions;
    std::vector<std::shared_ptr<client_session_t>> connected_sessions;

    std::mutex connected_mu;

    std::mutex connect_sched_mu;
    std::atomic<size_t> next_connect_idx;
    std::atomic<long> connect_inflight;

    std::atomic<long> connect_success;
    std::atomic<long> connect_fail;
    std::atomic<long> connect_completed;

    std::atomic<long> sent_msgs;
    std::atomic<long> recv_msgs;
    std::atomic<long> parse_error;
    std::atomic<long> timeout_error;
    std::atomic<long> send_error;
    std::atomic<long> outstanding_total;
    std::atomic<long> abandoned_outstanding;
    std::atomic<uint64_t> seq_gen;

    std::atomic<bool> measuring;
    std::chrono::steady_clock::time_point measure_start;
    std::chrono::steady_clock::time_point measure_end;

    std::vector<double> rtt_samples_us;
    std::atomic<size_t> sample_overwrite_idx;
    loopback_bind_plan_t loopback_bind_plan;

    std::mutex connect_mu;
    std::condition_variable connect_cv;

    tcp::endpoint endpoint;
};

client_session_t::client_session_t (echo_client_t &owner_,
                                    boost::asio::io_context &io_,
                                    const tcp::endpoint &source_bind_ep_) :
    owner (owner_),
    socket (io_),
    retry_timer (io_),
    strand (boost::asio::make_strand (io_)),
    connect_reported (false),
    connected_flag (false),
    closed (false),
    write_inflight (false),
    connect_started (false),
    outstanding (0),
    connect_deadline (),
    endpoint (),
    source_bind_ep (source_bind_ep_),
    read_header (stream_echo::k_stream_packet_prefix_size, 0),
    read_body (owner_.options ().size, 0),
    write_buf (),
    read_header_size (0)
{
    stream_echo::build_frame (NULL, owner.options ().size, &write_buf);
    for (size_t i =
           stream_echo::k_stream_packet_prefix_size + stream_echo::k_stream_msg_name_size + 16;
         i < write_buf.size (); ++i)
        write_buf[i] = 0xA5;
}

bool client_session_t::connected () const
{
    return connected_flag && !closed;
}

void client_session_t::begin_connect (const tcp::endpoint &endpoint_)
{
    endpoint = endpoint_;
    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::post (strand, [self] () {
        if (self->closed)
            return;

        if (self->connect_reported && self->connected ()) {
            self->maybe_send_more ();
            return;
        }

        if (!self->connect_started)
            self->do_connect ();
    });
}

void client_session_t::start_traffic ()
{
    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::post (strand, [self] () {
        if (self->closed || !self->connected ())
            return;
        self->maybe_send_more ();
    });
}

void client_session_t::do_connect ()
{
    connect_started = true;
    if (connect_deadline.time_since_epoch ().count () == 0) {
        connect_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (k_connect_timeout_s);
    }
    boost::system::error_code ec;

    if (socket.is_open ())
        socket.close (ec);

    socket.open (tcp::v4 (), ec);
    if (ec) {
        on_connect (ec);
        return;
    }

    if (source_bind_ep.address ().is_v4 ()
        && !source_bind_ep.address ().to_v4 ().is_unspecified ()) {
        socket.bind (source_bind_ep, ec);
        if (ec) {
            // Fallback to kernel source address selection if bind fails.
            ec.clear ();
        }
    }

    const std::shared_ptr<client_session_t> self = shared_from_this ();
    socket.async_connect (
      endpoint, boost::asio::bind_executor (
                  strand, [self] (const boost::system::error_code &e) { self->on_connect (e); }));
}

void client_session_t::on_connect (const boost::system::error_code &ec)
{
    if (closed || connect_reported)
        return;

    if (!ec) {
        connected_flag = true;
        connect_reported = true;
        retry_timer.cancel ();
        apply_socket_tuning ();
        owner.on_connect_result (true, shared_from_this ());
        start_read_header ();
        maybe_send_more ();
        return;
    }

    if (std::chrono::steady_clock::now () < connect_deadline) {
        retry_timer.expires_after (std::chrono::milliseconds (k_connect_retry_delay_ms));
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        retry_timer.async_wait (
          boost::asio::bind_executor (strand, [self] (const boost::system::error_code &wait_ec) {
              if (wait_ec || self->closed)
                  return;
              self->do_connect ();
          }));
        return;
    }

    connect_reported = true;
    owner.on_connect_result (false, shared_from_this ());
    close_internal ();
}

void client_session_t::apply_socket_tuning ()
{
    boost::system::error_code ec;
    socket.set_option (boost::asio::ip::tcp::no_delay (k_socket_tcp_nodelay != 0), ec);
    boost::asio::socket_base::send_buffer_size snd (k_socket_sndbuf);
    socket.set_option (snd, ec);
    boost::asio::socket_base::receive_buffer_size rcv (k_socket_rcvbuf);
    socket.set_option (rcv, ec);
}

void client_session_t::start_read_header ()
{
    if (closed || !connected ())
        return;

    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::async_read (socket, boost::asio::buffer (read_header),
                             boost::asio::bind_executor (
                               strand, [self] (const boost::system::error_code &ec, size_t bytes) {
                                   self->on_read_header (ec, bytes);
                               }));
}

void client_session_t::on_read_header (const boost::system::error_code &ec, size_t bytes)
{
    if (closed)
        return;

    if (ec || bytes != stream_echo::k_stream_packet_prefix_size) {
        close_internal ();
        return;
    }

    read_header_size = static_cast<size_t> (stream_echo::load_u16_be (&read_header[0]));
    const size_t len = static_cast<size_t> (stream_echo::load_u32_be (&read_header[2]));
    if (!stream_echo::valid_frame_sizes (read_header_size, len) || len < k_min_payload_size
        || len > k_max_payload_size) {
        owner.add_parse_error ();
        close_internal ();
        return;
    }

    start_read_body (read_header_size + len);
}

void client_session_t::start_read_body (size_t len)
{
    if (closed || !connected ())
        return;

    if (len != (stream_echo::k_stream_msg_name_size + owner.options ().size)) {
        owner.add_parse_error ();
        close_internal ();
        return;
    }
    if (read_body.size () != len)
        read_body.resize (len);
    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::async_read (socket, boost::asio::buffer (&read_body[0], len),
                             boost::asio::bind_executor (
                               strand, [self] (const boost::system::error_code &ec, size_t bytes) {
                                   self->on_read_body (ec, bytes);
                               }));
}

void client_session_t::on_read_body (const boost::system::error_code &ec, size_t bytes)
{
    if (closed)
        return;

    if (ec || bytes != read_body.size ()) {
        close_internal ();
        return;
    }

    if (read_body.size () != (stream_echo::k_stream_msg_name_size + owner.options ().size)) {
        owner.add_parse_error ();
    }

    if (!stream_echo::is_msg_name (&read_body[0], read_header_size)) {
        owner.add_parse_error ();
    }

    const unsigned char *payload = &read_body[static_cast<size_t> (read_header_size)];
    const size_t payload_size = read_body.size () - read_header_size;

    if (outstanding > 0) {
        outstanding -= 1;
        owner.mark_recv_success ();
    } else {
        // Late echo can arrive while shutting down; count only if traffic is active.
        if (owner.allow_new_send ())
            owner.add_parse_error ();
    }

    if (payload_size >= 16) {
        const uint64_t seq = stream_echo::load_u64_be (payload);
        const uint64_t sent_ns = stream_echo::load_u64_be (payload + 8);
        if (seq > 0 && sent_ns > 0 && (seq % k_rtt_sample_rate) == 0) {
            const uint64_t now_ns = stream_echo::now_ns ();
            if (now_ns > sent_ns) {
                const double us = static_cast<double> (now_ns - sent_ns) / 1000.0;
                owner.add_rtt_sample (us);
            }
        }
    }

    maybe_send_more ();
    start_read_header ();
}

void client_session_t::maybe_send_more ()
{
    if (closed || !connected ())
        return;

    if (write_inflight)
        return;

    if (!owner.allow_new_send ())
        return;

    if (outstanding >= static_cast<size_t> (std::max (1, owner.options ().inflight)))
        return;

    send_one ();
}

void client_session_t::send_one ()
{
    const uint64_t seq = owner.next_seq ();
    const uint64_t sent_ns = stream_echo::now_ns ();
    const size_t payload_offset =
      stream_echo::k_stream_packet_prefix_size + stream_echo::k_stream_msg_name_size;
    stream_echo::store_u64_be (&write_buf[payload_offset], seq);
    stream_echo::store_u64_be (&write_buf[payload_offset + 8], sent_ns);

    owner.mark_send_success ();
    outstanding += 1;

    write_inflight = true;
    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::async_write (
      socket, boost::asio::buffer (write_buf),
      boost::asio::bind_executor (strand, [self] (const boost::system::error_code &ec,
                                                  size_t bytes) { self->on_write (ec, bytes); }));
}

void client_session_t::on_write (const boost::system::error_code &ec, size_t bytes)
{
    write_inflight = false;

    if (closed)
        return;

    if (ec || bytes != write_buf.size ()) {
        owner.add_send_error ();
        close_internal ();
        return;
    }

    maybe_send_more ();
}

void client_session_t::request_close ()
{
    const std::shared_ptr<client_session_t> self = shared_from_this ();
    boost::asio::post (strand, [self] () { self->close_internal (); });
}

void client_session_t::close_internal ()
{
    if (closed)
        return;
    closed = true;

    if (outstanding > 0)
        owner.mark_abandoned (outstanding);
    outstanding = 0;

    boost::system::error_code ec;
    retry_timer.cancel ();
    socket.cancel (ec);
    socket.shutdown (tcp::socket::shutdown_both, ec);
    socket.close (ec);
}

bool parse_options (int argc, char **argv, client_options_t &opt)
{
    const stream_echo::arg_reader_t args (argc, argv);

    opt.host = args.get_string ("--host", opt.host.c_str ());
    opt.port = args.get_int ("--port", opt.port, 1);
    opt.ccu = args.get_int ("--ccu", opt.ccu, 1);
    opt.size = args.get_size ("--size", opt.size, k_min_payload_size);
    opt.duration = args.get_int ("--duration", opt.duration, 1);
    opt.inflight = args.get_int ("--inflight", opt.inflight, 1);
    opt.io_threads = args.get_int ("--io-threads", opt.io_threads, 1);
    if (opt.size > k_max_payload_size) {
        std::fprintf (stderr, "size too large: %zu\n", opt.size);
        return false;
    }

    return true;
}

} // namespace

int run_stream_client (int argc, char **argv)
{
    client_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    echo_client_t app (opt);
    return app.run ();
}

} // namespace stream_client
