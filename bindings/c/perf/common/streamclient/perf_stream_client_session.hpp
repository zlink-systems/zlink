#ifndef PERF_STREAM_CLIENT_SESSION_HPP
#define PERF_STREAM_CLIENT_SESSION_HPP

// Async transport session for the multi-connection benchmark path.
// Provides:
//   Benchmark tuning constants (connect batch, socket buffers, RTT capacity)
//   phase_mode_t  - ready / active state enum
//   session_latch_t – condition-variable barrier for strand acknowledgements
//   client_session_t – single transport connection with packet-framed async I/O loop
//
// Each client_session_t runs entirely on a strand and reports events
// back to the bench_client_t orchestrator via the bench_client_iface_t
// interface, keeping the session decoupled from the benchmark logic.

#include "perf_stream_bench_client_iface.hpp"
#include "perf_stream_common.hpp"
#include "perf_stream_frame_reassembly.hpp"
#include "../../multi/common/perf_multi_metric_header.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// --- Benchmark tuning constants ---

inline constexpr int k_connect_batch = 1024;                 // max concurrent connect() calls
inline constexpr int k_connect_timeout_s = 90;               // connect phase timeout budget
inline constexpr int k_resize_timeout_s = 30;                // chunk-size barrier wait limit
inline constexpr int k_socket_sndbuf_default = 64 * 1024;    // SO_SNDBUF default (64 KiB)
inline constexpr int k_socket_rcvbuf_default = 64 * 1024;    // SO_RCVBUF default (64 KiB)
inline constexpr int k_socket_tcp_nodelay = 1;               // TCP_NODELAY on
inline constexpr size_t k_rtt_sample_capacity = 1024 * 1024; // max RTT samples (1M)
inline constexpr size_t k_loopback_port_headroom = 64;       // ports reserved for OS use

inline int resolve_stream_sockbuf_env (const char *name, int default_bytes)
{
    if (!name || !*name)
        return default_bytes;

    const char *raw = std::getenv (name);
    if (!raw || !*raw)
        return default_bytes;

    char *end = NULL;
    errno = 0;
    const long parsed = std::strtol (raw, &end, 10);
    if (errno != 0 || end == raw || parsed <= 0)
        return default_bytes;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline int resolve_stream_sndbuf_bytes ()
{
    static const int value =
      resolve_stream_sockbuf_env ("PERF_MULTI_STREAM_SOCKET_SNDBUF", k_socket_sndbuf_default);
    return value;
}

inline int resolve_stream_rcvbuf_bytes ()
{
    static const int value =
      resolve_stream_sockbuf_env ("PERF_MULTI_STREAM_SOCKET_RCVBUF", k_socket_rcvbuf_default);
    return value;
}

// Benchmark phase: controls whether sessions are ready or actively sending.
enum phase_mode_t
{
    phase_ready = 0,  // no traffic
    phase_active = 1, // traffic active, metrics collected
};

// Countdown barrier for operations dispatched to every connected session.
struct session_latch_t
{
    explicit session_latch_t (size_t pending_) : pending (pending_) {}

    std::mutex mu;
    std::condition_variable cv;
    size_t pending; // decremented by each session via strand dispatch
};

enum raw_transport_mode_t
{
    raw_transport_tcp = 0,
    raw_transport_tls = 1,
    raw_transport_ws = 2,
    raw_transport_wss = 3,
};

inline raw_transport_mode_t parse_transport_mode (const std::string &transport)
{
    const std::string t = perf_stream_common::lower_copy (transport);
    if (t == "tls")
        return raw_transport_tls;
    if (t == "ws")
        return raw_transport_ws;
    if (t == "wss")
        return raw_transport_wss;
    return raw_transport_tcp;
}

// Single async transport connection that runs a packet-framed echo loop.
//
// Lifecycle:
//   begin_connect() → do_connect() → on_connect()
//       → start_traffic() → maybe_send_more() → send_one() [async_write]
//       → on_write() → start_read_header() → on_read_header()
//       → start_read_payload() → on_read_payload() → maybe_send_more() [loop]
//
// All I/O callbacks run on the session's strand for serialization.
// The session reports events to the orchestrator via bench_client_iface_t.
class client_session_t : public std::enable_shared_from_this<client_session_t>
{
  public:
    client_session_t (bench_client_iface_t &owner_,
                      boost::asio::io_context &io_,
                      const std::string &transport_,
                      const boost::asio::ip::tcp::endpoint &source_bind_ep_) :
        owner (owner_),
        io (io_),
        transport_mode (parse_transport_mode (transport_)),
        tls_ctx (boost::asio::ssl::context::tls_client),
        tcp_socket (),
        tls_socket (),
        ws_socket (),
        wss_socket (),
        ws_read_buffer (),
        ws_pending_frame (),
        strand (boost::asio::make_strand (io_)),
        closed (false),
        connected_flag (false),
        connect_started (false),
        connect_reported (false),
        write_pending (false),
        read_pending (false),
        outstanding (0),
        chunk_size (64),
        endpoint (),
        source_bind_ep (source_bind_ep_),
        write_buf (),
        read_header_declared (0),
        read_declared (0),
        read_buf ()
    {
        tls_ctx.set_verify_mode (boost::asio::ssl::verify_none);
        std::memset (read_header, 0, sizeof (read_header));
    }

    // Initiate async connect (posted to strand for thread-safety).
    void begin_connect (const boost::asio::ip::tcp::endpoint &endpoint_)
    {
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        boost::asio::post (strand, [self, endpoint_] () {
            if (self->closed)
                return;
            self->endpoint = endpoint_;
            if (!self->connect_started)
                self->do_connect ();
        });
    }

    // Update payload size and decrement the resize latch (strand-dispatched).
    void set_chunk_size (size_t size, const std::shared_ptr<session_latch_t> &latch)
    {
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        boost::asio::post (strand, [self, size, latch] () {
            if (!self->closed)
                self->chunk_size = size;

            if (latch) {
                {
                    std::lock_guard<std::mutex> lk (latch->mu);
                    if (latch->pending > 0)
                        --latch->pending;
                }
                latch->cv.notify_one ();
            }
        });
    }

    // Acknowledge that every handler admitted before phase_ready has left this
    // strand. Handlers queued after this point observe phase_ready and cannot
    // submit another echo.
    void acknowledge_phase_quiesced (const std::shared_ptr<session_latch_t> &latch)
    {
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        boost::asio::post (strand, [self, latch] () {
            if (!latch)
                return;
            {
                std::lock_guard<std::mutex> lk (latch->mu);
                if (latch->pending > 0)
                    --latch->pending;
            }
            latch->cv.notify_one ();
        });
    }

    // Kick the send loop when the active window starts.
    void start_traffic ()
    {
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        boost::asio::post (strand, [self] () {
            if (self->closed || !self->connected ())
                return;
            self->maybe_send_more ();
        });
    }

    // Graceful shutdown: cancel pending I/O and close the socket.
    void request_close ()
    {
        const std::shared_ptr<client_session_t> self = shared_from_this ();
        boost::asio::post (strand, [self] () { self->close_internal (); });
    }

    bool connected () const { return connected_flag && !closed; }

  private:
    // --- Connect ---

    std::string websocket_host_header () const
    {
        if (!endpoint.address ().is_unspecified ()) {
            std::string host = endpoint.address ().to_string ();
            if (endpoint.address ().is_v6 ())
                host = "[" + host + "]";
            return host + ":" + std::to_string (endpoint.port ());
        }
        return std::string ("127.0.0.1:0");
    }

    void bind_source_if_needed (boost::asio::ip::tcp::socket &socket, boost::system::error_code &ec)
    {
        if (!endpoint.address ().is_v4 ())
            return;
        if (!source_bind_ep.address ().is_v4 ())
            return;
        if (source_bind_ep.address ().to_v4 ().is_unspecified ())
            return;
        socket.bind (source_bind_ep, ec);
        if (ec)
            ec.clear ();
    }

    void on_tls_connected (const boost::system::error_code &ec)
    {
        if (closed || connect_reported)
            return;
        if (ec || !tls_socket) {
            on_connect (ec);
            return;
        }

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        tls_socket->async_handshake (
          boost::asio::ssl::stream_base::client,
          boost::asio::bind_executor (strand,
                                      [self] (const boost::system::error_code &handshake_ec) {
                                          self->on_connect (handshake_ec);
                                      }));
    }

    void on_ws_tcp_connected (const boost::system::error_code &ec)
    {
        if (closed || connect_reported)
            return;
        if (ec || !ws_socket) {
            on_connect (ec);
            return;
        }

        ws_socket->binary (true);
        ws_socket->set_option (boost::beast::websocket::stream_base::timeout::suggested (
          boost::beast::role_type::client));

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        ws_socket->async_handshake (
          websocket_host_header (), "/",
          boost::asio::bind_executor (strand,
                                      [self] (const boost::system::error_code &handshake_ec) {
                                          self->on_connect (handshake_ec);
                                      }));
    }

    void on_wss_tls_handshake (const boost::system::error_code &ec)
    {
        if (closed || connect_reported)
            return;
        if (ec || !wss_socket) {
            on_connect (ec);
            return;
        }

        wss_socket->binary (true);
        wss_socket->set_option (boost::beast::websocket::stream_base::timeout::suggested (
          boost::beast::role_type::client));

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        wss_socket->async_handshake (
          websocket_host_header (), "/",
          boost::asio::bind_executor (strand,
                                      [self] (const boost::system::error_code &handshake_ec) {
                                          self->on_connect (handshake_ec);
                                      }));
    }

    void on_wss_tcp_connected (const boost::system::error_code &ec)
    {
        if (closed || connect_reported)
            return;
        if (ec || !wss_socket) {
            on_connect (ec);
            return;
        }

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        wss_socket->next_layer ().async_handshake (
          boost::asio::ssl::stream_base::client,
          boost::asio::bind_executor (strand,
                                      [self] (const boost::system::error_code &handshake_ec) {
                                          self->on_wss_tls_handshake (handshake_ec);
                                      }));
    }

    // Open socket, optionally bind to a loopback shard address, and async_connect.
    void do_connect ()
    {
        using boost::asio::ip::tcp;

        connect_started = true;

        close_transport_socket ();

        boost::system::error_code ec;
        const std::shared_ptr<client_session_t> self = shared_from_this ();

        if (transport_mode == raw_transport_tcp) {
            // Allocate transport objects during connect setup, before any
            // active benchmark traffic is allowed on the session.
            tcp_socket.reset (new tcp::socket (io));
            tcp_socket->open (endpoint.protocol (), ec);
            if (ec) {
                on_connect (ec);
                return;
            }
            bind_source_if_needed (*tcp_socket, ec);
            tcp_socket->async_connect (
              endpoint,
              boost::asio::bind_executor (
                strand, [self] (const boost::system::error_code &e) { self->on_connect (e); }));
            return;
        }

        if (transport_mode == raw_transport_tls) {
            // TLS stream allocation is also a one-time setup cost.
            tls_socket.reset (new boost::asio::ssl::stream<tcp::socket> (io, tls_ctx));
            tls_socket->next_layer ().open (endpoint.protocol (), ec);
            if (ec) {
                on_connect (ec);
                return;
            }
            bind_source_if_needed (tls_socket->next_layer (), ec);
            tls_socket->next_layer ().async_connect (
              endpoint,
              boost::asio::bind_executor (strand, [self] (const boost::system::error_code &e) {
                  self->on_tls_connected (e);
              }));
            return;
        }

        if (transport_mode == raw_transport_ws) {
            ws_socket.reset (new boost::beast::websocket::stream<tcp::socket> (io));
            ws_socket->next_layer ().open (endpoint.protocol (), ec);
            if (ec) {
                on_connect (ec);
                return;
            }
            bind_source_if_needed (ws_socket->next_layer (), ec);
            ws_socket->next_layer ().async_connect (
              endpoint,
              boost::asio::bind_executor (strand, [self] (const boost::system::error_code &e) {
                  self->on_ws_tcp_connected (e);
              }));
            return;
        }

        wss_socket.reset (
          new boost::beast::websocket::stream<boost::asio::ssl::stream<tcp::socket>> (io, tls_ctx));
        wss_socket->next_layer ().next_layer ().open (endpoint.protocol (), ec);
        if (ec) {
            on_connect (ec);
            return;
        }
        bind_source_if_needed (wss_socket->next_layer ().next_layer (), ec);
        wss_socket->next_layer ().next_layer ().async_connect (
          endpoint,
          boost::asio::bind_executor (strand, [self] (const boost::system::error_code &e) {
              self->on_wss_tcp_connected (e);
          }));
    }

    // Handle connect result: apply tuning on success or fail immediately.
    void on_connect (const boost::system::error_code &ec)
    {
        if (closed || connect_reported)
            return;

        if (!ec) {
            connected_flag = true;
            connect_reported = true;
            apply_socket_tuning ();
            owner.on_connect_result (true, shared_from_this ());
            return;
        }

        if (std::getenv ("PERF_DEBUG")) {
            std::fprintf (stderr, "perf_stream_client: connect_error code=%d message=%s\n",
                          ec.value (), ec.message ().c_str ());
        }
        connect_reported = true;
        owner.on_connect_result (false, shared_from_this ());
        close_internal ();
    }

    // --- Send/receive echo loop ---

    // Keep one unresolved echo per raw connection. A raw peer has no Core HWM
    // admission point, so using async_write completion as admission would fill
    // transport-specific OS/TLS/WebSocket buffers and distort comparisons.
    void maybe_send_more ()
    {
        if (closed || !connected ())
            return;
        if (write_pending)
            return;
        if (!perf_stream_common::perf_stream_can_submit_echo (outstanding))
            return;
        if (!owner.allow_send ())
            return;
        send_one ();
    }

    // Build a packet-framed payload, stamp the metric header into the body,
    // and async_write.
    void send_one ()
    {
        const size_t size = owner.current_phase_size ();
        if (size < perf_stream_common::k_stream_min_chunk_size
            || size > perf_stream_common::k_stream_max_chunk_size)
            return;

        chunk_size = size;
        const size_t wire_size = perf_stream_common::k_stream_packet_prefix_size
                                 + perf_stream_common::k_stream_msg_name_size + size;
        if (write_buf.size () != wire_size)
            write_buf.resize (wire_size);

        perf_stream_common::perf_stream_store_u16_be (
          &write_buf[0], static_cast<uint16_t> (perf_stream_common::k_stream_msg_name_size));
        perf_stream_common::perf_stream_store_u32_be (&write_buf[2], static_cast<uint32_t> (size));
        std::memcpy (&write_buf[perf_stream_common::k_stream_packet_prefix_size],
                     perf_stream_common::k_stream_msg_name,
                     perf_stream_common::k_stream_msg_name_size);
        unsigned char *payload_write =
          write_buf.size () > (perf_stream_common::k_stream_packet_prefix_size
                               + perf_stream_common::k_stream_msg_name_size)
            ? &write_buf[perf_stream_common::k_stream_packet_prefix_size
                         + perf_stream_common::k_stream_msg_name_size]
            : NULL;

        if (payload_write && size >= perf_multi_metric::header_size ()) {
            const uint64_t seq = owner.next_seq ();
            // STREAM echoes return this timestamp to the same client process.
            // Use the same monotonic clock as the active cutoff so wall-clock
            // adjustments cannot inflate an in-window RTT.
            const uint64_t sent_ts_ns = perf_stream_common::perf_stream_now_ns ();
            (void) perf_multi_metric::stamp_payload (payload_write, size, owner.metric_run_id (),
                                                     owner.metric_phase (), size, seq, sent_ts_ns);
        }

        owner.on_send_begin (size);
        ++outstanding;
        write_pending = true;

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        if (transport_mode == raw_transport_tcp && tcp_socket) {
            boost::asio::async_write (
              *tcp_socket, boost::asio::buffer (write_buf),
              boost::asio::bind_executor (strand,
                                          [self] (const boost::system::error_code &err,
                                                  size_t bytes) { self->on_write (err, bytes); }));
            return;
        }

        if (transport_mode == raw_transport_tls && tls_socket) {
            boost::asio::async_write (
              *tls_socket, boost::asio::buffer (write_buf),
              boost::asio::bind_executor (strand,
                                          [self] (const boost::system::error_code &err,
                                                  size_t bytes) { self->on_write (err, bytes); }));
            return;
        }

        if (transport_mode == raw_transport_ws && ws_socket) {
            ws_socket->async_write (
              boost::asio::buffer (write_buf),
              boost::asio::bind_executor (strand,
                                          [self] (const boost::system::error_code &err,
                                                  size_t bytes) { self->on_write (err, bytes); }));
            return;
        }

        if (transport_mode == raw_transport_wss && wss_socket) {
            wss_socket->async_write (
              boost::asio::buffer (write_buf),
              boost::asio::bind_executor (strand,
                                          [self] (const boost::system::error_code &err,
                                                  size_t bytes) { self->on_write (err, bytes); }));
            return;
        }

        on_write (boost::asio::error::make_error_code (boost::asio::error::not_connected), 0);
    }

    void on_write (const boost::system::error_code &ec, size_t bytes)
    {
        write_pending = false;

        if (closed)
            return;

        if (ec || bytes != write_buf.size ()) {
            owner.on_send_error ();
            if (outstanding > 0) {
                --outstanding;
                owner.on_abandon (1);
            }
            close_internal ();
            return;
        }

        start_read_header ();
        maybe_send_more ();
    }

    // Read the 6-byte packet prefix from the echo server.
    void start_read_header ()
    {
        if (closed || !connected ())
            return;
        if (read_pending)
            return;

        read_pending = true;

        if (transport_mode == raw_transport_ws || transport_mode == raw_transport_wss) {
            start_read_ws_frame ();
            return;
        }

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        if (transport_mode == raw_transport_tcp && tcp_socket) {
            boost::asio::async_read (
              *tcp_socket, boost::asio::buffer (read_header),
              boost::asio::bind_executor (
                strand, [self] (const boost::system::error_code &err, size_t bytes) {
                    self->on_read_header (err, bytes);
                }));
            return;
        }
        if (transport_mode == raw_transport_tls && tls_socket) {
            boost::asio::async_read (
              *tls_socket, boost::asio::buffer (read_header),
              boost::asio::bind_executor (
                strand, [self] (const boost::system::error_code &err, size_t bytes) {
                    self->on_read_header (err, bytes);
                }));
            return;
        }

        on_read_header (boost::asio::error::make_error_code (boost::asio::error::not_connected), 0);
    }

    // Read the packet body (size declared in the prefix).
    void start_read_payload ()
    {
        if (closed || !connected ())
            return;

        if (!perf_stream_common::perf_stream_validate_frame_sizes (read_header_declared,
                                                                   read_declared)) {
            owner.on_recv_error ();
            if (outstanding > 0) {
                const long dropped = static_cast<long> (outstanding);
                outstanding = 0;
                owner.on_abandon (dropped);
            }
            close_internal ();
            return;
        }

        const size_t read_total_size =
          static_cast<size_t> (read_header_declared) + static_cast<size_t> (read_declared);
        if (read_buf.size () != read_total_size)
            read_buf.resize (read_total_size);

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        if (transport_mode == raw_transport_tcp && tcp_socket) {
            boost::asio::async_read (
              *tcp_socket, boost::asio::buffer (read_buf),
              boost::asio::bind_executor (
                strand, [self] (const boost::system::error_code &err, size_t bytes) {
                    self->on_read_payload (err, bytes);
                }));
            return;
        }
        if (transport_mode == raw_transport_tls && tls_socket) {
            boost::asio::async_read (
              *tls_socket, boost::asio::buffer (read_buf),
              boost::asio::bind_executor (
                strand, [self] (const boost::system::error_code &err, size_t bytes) {
                    self->on_read_payload (err, bytes);
                }));
            return;
        }

        on_read_payload (boost::asio::error::make_error_code (boost::asio::error::not_connected),
                         0);
    }

    void on_ws_read (const boost::system::error_code &ec, size_t)
    {
        if (closed)
            return;

        if (ec) {
            if (std::getenv ("PERF_DEBUG")) {
                std::fprintf (stderr, "perf_stream_client: ws_read_error code=%d message=%s\n",
                              ec.value (), ec.message ().c_str ());
            }
            owner.on_recv_error ();
            if (outstanding > 0) {
                const long dropped = static_cast<long> (outstanding);
                outstanding = 0;
                owner.on_abandon (dropped);
            }
            close_internal ();
            return;
        }

        const size_t bytes = boost::asio::buffer_size (ws_read_buffer.data ());
        if (bytes > 0) {
            const size_t base = ws_pending_frame.bytes.size ();
            ws_pending_frame.bytes.resize (base + bytes);
            boost::asio::buffer_copy (boost::asio::buffer (&ws_pending_frame.bytes[base], bytes),
                                      ws_read_buffer.data ());
        }
        ws_read_buffer.consume (bytes);

        start_read_ws_frame ();
    }

    void start_read_ws_frame ()
    {
        if (closed || !connected ())
            return;

        if (perf_stream_frame::has_invalid_declared_size (&ws_pending_frame)) {
            owner.on_recv_error ();
            if (outstanding > 0) {
                const long dropped = static_cast<long> (outstanding);
                outstanding = 0;
                owner.on_abandon (dropped);
            }
            close_internal ();
            return;
        }

        perf_stream_frame::frame_view_t frame;
        if (perf_stream_frame::try_peek (&ws_pending_frame, &frame)) {
            read_header_declared = static_cast<uint16_t> (frame.header_size);
            read_declared = static_cast<uint32_t> (frame.payload_size);
            const size_t total_size =
              static_cast<size_t> (read_header_declared) + static_cast<size_t> (read_declared);
            if (read_buf.size () != total_size)
                read_buf.resize (total_size);
            if (frame.header_size > 0) {
                std::memcpy (&read_buf[0], frame.header, frame.header_size);
            }
            if (read_declared > 0) {
                std::memcpy (&read_buf[static_cast<size_t> (read_header_declared)], frame.payload,
                             static_cast<size_t> (read_declared));
            }
            perf_stream_frame::consume (&ws_pending_frame, frame);
            perf_stream_frame::compact (&ws_pending_frame);

            on_read_payload (boost::system::error_code (), total_size);
            return;
        }

        const std::shared_ptr<client_session_t> self = shared_from_this ();
        if (transport_mode == raw_transport_ws && ws_socket) {
            ws_socket->async_read (
              ws_read_buffer, boost::asio::bind_executor (
                                strand, [self] (const boost::system::error_code &err,
                                                size_t bytes) { self->on_ws_read (err, bytes); }));
            return;
        }
        if (transport_mode == raw_transport_wss && wss_socket) {
            wss_socket->async_read (
              ws_read_buffer, boost::asio::bind_executor (
                                strand, [self] (const boost::system::error_code &err,
                                                size_t bytes) { self->on_ws_read (err, bytes); }));
            return;
        }

        on_ws_read (boost::asio::error::make_error_code (boost::asio::error::not_connected), 0);
    }

    void on_read_header (const boost::system::error_code &ec, size_t bytes)
    {
        if (closed)
            return;

        if (ec || bytes != sizeof (read_header)) {
            if (std::getenv ("PERF_DEBUG")) {
                std::fprintf (stderr,
                              "perf_stream_client: read_header_error code=%d message=%s bytes=%zu "
                              "expected=%zu\n",
                              ec.value (), ec.message ().c_str (), bytes, sizeof (read_header));
            }
            owner.on_recv_error ();
            if (outstanding > 0) {
                const long dropped = static_cast<long> (outstanding);
                outstanding = 0;
                owner.on_abandon (dropped);
            }
            close_internal ();
            return;
        }

        const uint16_t header_size = perf_stream_common::perf_stream_load_u16_be (read_header);
        read_header_declared = header_size;
        read_declared = perf_stream_common::perf_stream_load_u32_be (read_header + 2);
        start_read_payload ();
    }

    // Complete a roundtrip: decode stamped body header, report to
    // orchestrator, and re-enter the send loop.
    void on_read_payload (const boost::system::error_code &ec, size_t bytes)
    {
        read_pending = false;

        if (closed)
            return;

        const size_t expected_bytes =
          static_cast<size_t> (read_header_declared) + static_cast<size_t> (read_declared);
        if (ec || bytes != expected_bytes) {
            if (std::getenv ("PERF_DEBUG")) {
                std::fprintf (stderr,
                              "perf_stream_client: read_payload_error code=%d message=%s bytes=%zu "
                              "expected=%zu header=%u payload=%u\n",
                              ec.value (), ec.message ().c_str (), bytes, expected_bytes,
                              static_cast<unsigned> (read_header_declared),
                              static_cast<unsigned> (read_declared));
            }
            owner.on_recv_error ();
            if (outstanding > 0) {
                const long dropped = static_cast<long> (outstanding);
                outstanding = 0;
                owner.on_abandon (dropped);
            }
            close_internal ();
            return;
        }

        bool valid_measurement = true;
        if (!perf_stream_common::perf_stream_is_msg_name (bytes > 0 ? &read_buf[0] : NULL,
                                                          read_header_declared)) {
            owner.on_size_mismatch ();
            valid_measurement = false;
        }
        if (read_declared != static_cast<uint32_t> (chunk_size)) {
            owner.on_size_mismatch ();
            if (outstanding > 0) {
                --outstanding;
                owner.on_abandon (1);
            }
            maybe_send_more ();
            if (outstanding > 0)
                start_read_header ();
            return;
        }

        const size_t payload_bytes = static_cast<size_t> (read_declared);
        const unsigned char *payload_ptr =
          payload_bytes > 0 ? &read_buf[static_cast<size_t> (read_header_declared)] : NULL;

        uint64_t sent_ts_ns = 0;
        if (payload_ptr && payload_bytes >= perf_multi_metric::header_size ()) {
            perf_multi_metric::header_t header;
            if (perf_multi_metric::decode_payload_header (payload_ptr, payload_bytes, &header)
                && perf_multi_metric::is_expected (header, owner.metric_run_id (),
                                                    perf_multi_metric::phase_active,
                                                    chunk_size)
                && header.sent_ts_ns > 0) {
                sent_ts_ns = static_cast<uint64_t> (header.sent_ts_ns);
            } else {
                owner.on_size_mismatch ();
                valid_measurement = false;
            }
        } else {
            owner.on_size_mismatch ();
            valid_measurement = false;
        }

        if (outstanding > 0) {
            --outstanding;
            owner.on_recv_done (payload_bytes, valid_measurement ? sent_ts_ns : 0);
        }

        maybe_send_more ();
        if (outstanding > 0)
            start_read_header ();
    }

    // --- Cleanup ---

    // Cancel timers, shutdown socket, and report abandoned outstanding ops.
    void close_internal ()
    {
        if (closed)
            return;
        closed = true;

        if (outstanding > 0) {
            const long dropped = static_cast<long> (outstanding);
            outstanding = 0;
            owner.on_abandon (dropped);
        }

        boost::system::error_code ec;
        close_transport_socket ();
    }

    void close_transport_socket ()
    {
        using boost::asio::ip::tcp;

        boost::system::error_code ec;
        perf_stream_frame::reset (&ws_pending_frame);
        ws_read_buffer.consume (ws_read_buffer.size ());

        if (ws_socket) {
            ws_socket->next_layer ().cancel (ec);
            ws_socket->next_layer ().shutdown (tcp::socket::shutdown_both, ec);
            ws_socket->next_layer ().close (ec);
            ws_socket.reset ();
        }

        if (wss_socket) {
            wss_socket->next_layer ().next_layer ().cancel (ec);
            wss_socket->next_layer ().next_layer ().shutdown (tcp::socket::shutdown_both, ec);
            wss_socket->next_layer ().next_layer ().close (ec);
            wss_socket.reset ();
        }

        if (tls_socket) {
            tls_socket->next_layer ().cancel (ec);
            tls_socket->next_layer ().shutdown (tcp::socket::shutdown_both, ec);
            tls_socket->next_layer ().close (ec);
            tls_socket.reset ();
        }

        if (tcp_socket) {
            tcp_socket->cancel (ec);
            tcp_socket->shutdown (tcp::socket::shutdown_both, ec);
            tcp_socket->close (ec);
            tcp_socket.reset ();
        }
    }

    boost::asio::ip::tcp::socket *active_tcp_socket ()
    {
        if (transport_mode == raw_transport_tcp && tcp_socket)
            return tcp_socket.get ();
        if (transport_mode == raw_transport_tls && tls_socket)
            return &tls_socket->next_layer ();
        if (transport_mode == raw_transport_ws && ws_socket)
            return &ws_socket->next_layer ();
        if (transport_mode == raw_transport_wss && wss_socket)
            return &wss_socket->next_layer ().next_layer ();
        return NULL;
    }

    // Set TCP_NODELAY, SO_SNDBUF, SO_RCVBUF for low-latency benchmarking.
    void apply_socket_tuning ()
    {
        boost::asio::ip::tcp::socket *socket = active_tcp_socket ();
        if (!socket)
            return;

        boost::system::error_code ec;
        socket->set_option (boost::asio::ip::tcp::no_delay (k_socket_tcp_nodelay != 0), ec);
        boost::asio::socket_base::send_buffer_size snd (resolve_stream_sndbuf_bytes ());
        socket->set_option (snd, ec);
        boost::asio::socket_base::receive_buffer_size rcv (resolve_stream_rcvbuf_bytes ());
        socket->set_option (rcv, ec);
    }

    // --- Member state ---

    bench_client_iface_t &owner; // orchestrator (bench_client_t)
    boost::asio::io_context &io;
    raw_transport_mode_t transport_mode;
    boost::asio::ssl::context tls_ctx;
    std::unique_ptr<boost::asio::ip::tcp::socket> tcp_socket;
    std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> tls_socket;
    std::unique_ptr<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>> ws_socket;
    std::unique_ptr<
      boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>>
      wss_socket;
    boost::beast::flat_buffer ws_read_buffer;
    perf_stream_frame::buffer_t ws_pending_frame;

    boost::asio::strand<boost::asio::io_context::executor_type> strand; // serializes all callbacks

    bool closed;           // permanently shut down
    bool connected_flag;   // connect succeeded
    bool connect_started;  // do_connect() called at least once
    bool connect_reported; // on_connect_result() sent to owner
    bool write_pending;    // async_write in progress
    bool read_pending;     // async_read in progress
    size_t outstanding;    // submitted messages whose echoes are not drained yet

    size_t chunk_size;                             // current payload size
    boost::asio::ip::tcp::endpoint endpoint;       // server address
    boost::asio::ip::tcp::endpoint source_bind_ep; // loopback shard bind address

    std::vector<unsigned char> write_buf; // reusable send buffer [prefix + body]
    unsigned char read_header[perf_stream_common::k_stream_packet_prefix_size];
    uint16_t read_header_declared;       // header length from packet prefix
    uint32_t read_declared;              // body length from packet prefix
    std::vector<unsigned char> read_buf; // reusable receive buffer
};

#endif
