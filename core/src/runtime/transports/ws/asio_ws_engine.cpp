/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_WS

#include "transports/ws/asio_ws_engine.hpp"
#include "engine/asio/asio_stream_fastpath_policy.hpp"
#include "engine/asio/asio_poller.hpp"
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/config.hpp"
#include "utils/err.hpp"
#include "utils/heap_owner.hpp"
#include "utils/ip.hpp"
#include "transports/tcp/tcp.hpp"
#include "utils/likely.hpp"
#include "protocol/zmp_decoder.hpp"
#include "protocol/zmp_encoder.hpp"
#include "protocol/zmp_control.hpp"
#include "protocol/wire.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

#if defined ZLINK_HAVE_ASIO_SSL
#include <boost/asio/ssl.hpp>
#endif

#include <algorithm>
#include <cerrno>
#include <sstream>
#include <cstring>
#include <limits.h>

namespace
{
const bool ws_gather_write_on = zlink::asio_stream_fastpath_policy::gather_write_enabled ();

const size_t ws_gather_threshold = zlink::asio_stream_fastpath_policy::gather_threshold ();

// STREAM over WS/WSS has the same large-frame copy cost as TCP/TLS STREAM.
// Keep the STREAM-specific gather path enabled by default so 64 KiB echo
// traffic can write ZMP header + body without first copying the body through
// the encoder batch buffer. Plain WS users still need ZLINK_ASIO_GATHER_WRITE.
// Treat this as a STREAM hot path contract: future refactors must preserve the
// header/body gather route unless replacement perf numbers prove the copy path
// is no slower for WS/WSS 64 KiB echo traffic.
const bool ws_stream_gather_on = zlink::asio_stream_fastpath_policy::stream_gather_enabled ();

const size_t ws_stream_gather_threshold =
  zlink::asio_stream_fastpath_policy::stream_gather_threshold ();

const size_t stream_target_initial_cap = 64 * 1024;
const size_t ws_stream_initial_target = 64 * 1024;

size_t stream_decoder_initial_read_target (const zlink::options_t &options_)
{
    return zlink::asio_stream_fastpath_policy::decoder_initial_read_target (
      options_, ws_stream_initial_target, stream_target_initial_cap);
}

size_t stream_decoder_max_read_target (const zlink::options_t &options_)
{
    return zlink::asio_stream_fastpath_policy::decoder_max_read_target (
      options_, ws_stream_initial_target, stream_target_initial_cap);
}

size_t stream_encoder_initial_write_target (const zlink::options_t &options_)
{
    return zlink::asio_stream_fastpath_policy::encoder_initial_write_target (
      options_, ws_stream_initial_target, stream_target_initial_cap);
}

size_t stream_encoder_max_write_target (const zlink::options_t &options_)
{
    return zlink::asio_stream_fastpath_policy::encoder_max_write_target (
      options_, ws_stream_initial_target, stream_target_initial_cap);
}
}

//  Debug logging for ASIO WS engine - set to 1 to enable
#define ASIO_WS_ENGINE_DEBUG 0

#if ASIO_WS_ENGINE_DEBUG
#include <cstdio>
#define WS_ENGINE_DBG(fmt, ...) fprintf (stderr, "[ASIO_WS_ENGINE] " fmt "\n", ##__VA_ARGS__)
#else
#define WS_ENGINE_DBG(fmt, ...)
#endif

//  Message property names
#define ZLINK_MSG_PROPERTY_ROUTING_ID "Routing-Id"
#define ZLINK_MSG_PROPERTY_SOCKET_TYPE "Socket-Type"
#define ZLINK_MSG_PROPERTY_USER_ID "User-Id"
#define ZLINK_MSG_PROPERTY_PEER_ADDRESS "Peer-Address"

static std::string get_peer_address (zlink::fd_t s_)
{
    std::string peer_address;
    const int family = zlink::get_peer_ip_address (s_, peer_address);
    if (family == 0)
        peer_address.clear ();
    return peer_address;
}

zlink::asio_ws_engine_t::asio_ws_engine_t (fd_t fd_,
                                           const options_t &options_,
                                           const endpoint_uri_pair_t &endpoint_uri_pair_,
                                           bool is_client_,
                                           std::unique_ptr<i_asio_transport> transport_) :
    _transport (std::move (transport_)),
    _is_client (is_client_),
    _ws_handshake_complete (false),
    _session (NULL),
    _socket (NULL),
    _fd (fd_),
    _io_context (NULL),
    _callback_guard (std::shared_ptr<callback_guard_t> (new callback_guard_t ())),
    _options (options_),
    _endpoint_uri_pair (endpoint_uri_pair_),
    _peer_address (get_peer_address (fd_)),
    _inpos (NULL),
    _insize (0),
    _decoder (NULL),
    _input_in_decoder_buffer (false),
    _outpos (NULL),
    _outsize (0),
    _encoder (NULL),
    _next_msg (NULL),
    _process_msg (NULL),
    _metadata (NULL),
    _plugged (false),
    _handshaking (true),
    _input_stopped (false),
    _output_stopped (false),
    _io_error (false),
    _read_pending (false),
    _write_pending (false),
    _terminating (false),
    _last_error_code (0),
    _read_buffer (read_buffer_size),
    _read_buffer_ptr (NULL),
    _last_read_request_size (0),
    _last_read_had_partial_prefix (false),
    _stream_decoder_read_target_size (stream_decoder_initial_read_target (options_)),
    _stream_decoder_read_target_max (stream_decoder_max_read_target (options_)),
    _stream_decoder_read_target_full_hits (0),
    _stream_encoder_write_target_size (stream_encoder_initial_write_target (options_)),
    _stream_encoder_write_target_max (stream_encoder_max_write_target (options_)),
    _stream_encoder_write_target_full_hits (0),
    _stream_encoder_pending_resize_size (0),
    _async_gather (false),
    _gather_header_size (0),
    _gather_body (NULL),
    _gather_body_size (0),
    _hello_sent (false),
    _hello_received (false),
    _ready_sent (false),
    _ready_received (false),
    _hello_header_bytes (0),
    _hello_body_bytes (0),
    _hello_body_len (0),
    _hello_send_size (0),
    _negotiated_transport_lane (options_.transport_lane),
    _negotiated_transport_pair_id (options_.transport_pair_id),
    _negotiated_transport_pair_generation (
      options_.transport_pair_generation),
    _peer_routing_id_size (0),
    _subscription_required (false),
    _current_timer_id (-1),
    _has_handshake_timer (false)
{
    WS_ENGINE_DBG ("Constructor: fd=%d, client=%d", fd_, is_client_);

    int rc = _tx_msg.init ();
    errno_assert (rc == 0);

    rc = _routing_id_msg.init ();
    errno_assert (rc == 0);

    //  Initialize greeting buffers
    memset (_hello_recv, 0, sizeof (_hello_recv));
    memset (_hello_send, 0, sizeof (_hello_send));
    memset (_peer_routing_id, 0, sizeof (_peer_routing_id));

    zlink_assert (_transport);

    //  Put socket into non-blocking mode
    unblock_socket (_fd);
}

#if defined ZLINK_HAVE_ASIO_SSL
zlink::asio_ws_engine_t::asio_ws_engine_t (
  fd_t fd_,
  const options_t &options_,
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  bool is_client_,
  std::unique_ptr<i_asio_transport> transport_,
  std::unique_ptr<boost::asio::ssl::context> ssl_context_) :
    _transport (std::move (transport_)),
    _is_client (is_client_),
    _ws_handshake_complete (false),
    _session (NULL),
    _socket (NULL),
    _fd (fd_),
    _io_context (NULL),
    _callback_guard (std::shared_ptr<callback_guard_t> (new callback_guard_t ())),
    _options (options_),
    _endpoint_uri_pair (endpoint_uri_pair_),
    _peer_address (get_peer_address (fd_)),
    _inpos (NULL),
    _insize (0),
    _decoder (NULL),
    _input_in_decoder_buffer (false),
    _outpos (NULL),
    _outsize (0),
    _encoder (NULL),
    _next_msg (NULL),
    _process_msg (NULL),
    _metadata (NULL),
    _plugged (false),
    _handshaking (true),
    _input_stopped (false),
    _output_stopped (false),
    _io_error (false),
    _read_pending (false),
    _write_pending (false),
    _terminating (false),
    _last_error_code (0),
    _read_buffer (read_buffer_size),
    _read_buffer_ptr (NULL),
    _last_read_request_size (0),
    _last_read_had_partial_prefix (false),
    _stream_decoder_read_target_size (stream_decoder_initial_read_target (options_)),
    _stream_decoder_read_target_max (stream_decoder_max_read_target (options_)),
    _stream_decoder_read_target_full_hits (0),
    _stream_encoder_write_target_size (stream_encoder_initial_write_target (options_)),
    _stream_encoder_write_target_max (stream_encoder_max_write_target (options_)),
    _stream_encoder_write_target_full_hits (0),
    _stream_encoder_pending_resize_size (0),
    _async_gather (false),
    _gather_header_size (0),
    _gather_body (NULL),
    _gather_body_size (0),
    _hello_sent (false),
    _hello_received (false),
    _ready_sent (false),
    _ready_received (false),
    _hello_header_bytes (0),
    _hello_body_bytes (0),
    _hello_body_len (0),
    _hello_send_size (0),
    _negotiated_transport_lane (options_.transport_lane),
    _negotiated_transport_pair_id (options_.transport_pair_id),
    _negotiated_transport_pair_generation (
      options_.transport_pair_generation),
    _peer_routing_id_size (0),
    _subscription_required (false),
    _current_timer_id (-1),
    _has_handshake_timer (false),
    _ssl_context (std::move (ssl_context_))
{
    WS_ENGINE_DBG ("Constructor: fd=%d, client=%d (custom transport)", fd_, is_client_);

    int rc = _tx_msg.init ();
    errno_assert (rc == 0);

    rc = _routing_id_msg.init ();
    errno_assert (rc == 0);

    //  Initialize greeting buffers
    memset (_hello_recv, 0, sizeof (_hello_recv));
    memset (_hello_send, 0, sizeof (_hello_send));
    memset (_peer_routing_id, 0, sizeof (_peer_routing_id));

    zlink_assert (_transport);

    //  Put socket into non-blocking mode
    unblock_socket (_fd);
}
#endif

zlink::asio_ws_engine_t::~asio_ws_engine_t ()
{
    WS_ENGINE_DBG ("Destructor called");

    zlink_assert (!_plugged);

    //  Close transport (handles graceful close)
    if (_transport) {
        _transport->close ();
        _transport.reset ();
    }

    //  Close the underlying socket if still open
    if (_fd != retired_fd) {
#ifdef ZLINK_HAVE_WINDOWS
        int rc = closesocket (_fd);
        wsa_assert (rc != SOCKET_ERROR);
#else
        int rc = close (_fd);
#if defined(__FreeBSD_kernel__) || defined(__FreeBSD__)
        if (rc == -1 && errno == ECONNRESET)
            rc = 0;
#endif
        errno_assert (rc == 0);
#endif
        _fd = retired_fd;
    }

    int rc = _tx_msg.close ();
    errno_assert (rc == 0);

    rc = _routing_id_msg.close ();
    errno_assert (rc == 0);

    if (_metadata != NULL) {
        if (_metadata->drop_ref ()) {
            LIBZLINK_DELETE (_metadata);
        }
    }

    LIBZLINK_DELETE (_encoder);
    LIBZLINK_DELETE (_decoder);
}

void zlink::asio_ws_engine_t::plug (io_thread_t *io_thread_, session_base_t *session_)
{
    WS_ENGINE_DBG ("plug called");

    zlink_assert (!_plugged);
    _plugged = true;

    _session = session_;
    _socket = _session->get_socket ();

    //  Get reference to io_context
    asio_poller_t *poller = static_cast<asio_poller_t *> (io_thread_->get_poller ());
    _io_context = &poller->get_io_context ();

    //  Create timer
    _timer.reset (new boost::asio::steady_timer (*_io_context));

    //  Initialize WebSocket transport with the socket
    if (!_transport->open (*_io_context, _fd)) {
        WS_ENGINE_DBG ("Failed to open WebSocket transport");
        error (connection_error);
        return;
    }

    //  Mark FD as taken by transport (don't close it in destructor)
    _fd = retired_fd;

    _io_error = false;

    //  Start WebSocket handshake
    start_ws_handshake ();
}

void zlink::asio_ws_engine_t::start_ws_handshake ()
{
    WS_ENGINE_DBG ("Starting WebSocket handshake, is_client=%d", _is_client);

    //  Set handshake timer if configured
    if (_options.handshake_ivl > 0) {
        set_handshake_timer ();
    }

    //  Start WebSocket handshake (client or server)
    const int handshake_type = _is_client ? 0 : 1;
    const std::weak_ptr<callback_guard_t> callback_guard = _callback_guard;

    _transport->async_handshake (
      handshake_type, [this, callback_guard] (const boost::system::error_code &ec, std::size_t) {
          if (callback_guard.expired ())
              return;
          on_ws_handshake_complete (ec);
      });
}

void zlink::asio_ws_engine_t::on_ws_handshake_complete (const boost::system::error_code &ec)
{
    WS_ENGINE_DBG ("WebSocket handshake complete, ec=%s", ec.message ().c_str ());

    if (_terminating)
        return;

    if (ec) {
        WS_ENGINE_DBG ("WebSocket handshake failed");
        error (connection_error);
        return;
    }

    _ws_handshake_complete = true;

    //  Cancel handshake timer if running (we'll restart for ZMTP handshake)
    if (_has_handshake_timer) {
        cancel_handshake_timer ();
    }

    //  Start ZMP handshake over WebSocket
    start_zmp_handshake ();
}

void zlink::asio_ws_engine_t::start_zmp_handshake ()
{
    WS_ENGINE_DBG ("Starting ZMP handshake over WebSocket");

    if (_options.handshake_ivl > 0)
        set_handshake_timer ();

    _ready_sent = false;
    _ready_received = false;
    _ready_send.clear ();
    _deferred_ready_send.clear ();
    _deferred_ready_pending = false;
    _last_error_code = 0;
    _last_error_reason.clear ();

    const bool defer_ready =
      paired_transport () && !_options.transport_pair_initiator;
    if (defer_ready) {
        zmp_control::build_hello_frame (
          _options, _hello_send, sizeof (_hello_send), &_hello_send_size);
        _ready_send.assign (_hello_send, _hello_send + _hello_send_size);
    } else {
        zmp_control::build_hello_ready_frames (
          _options, _hello_send, sizeof (_hello_send), &_hello_send_size,
          _ready_send);
        _ready_sent = true;
    }
    _outpos = &_ready_send[0];
    _outsize = _ready_send.size ();
    _hello_sent = true;

    _hello_header_bytes = 0;
    _hello_body_bytes = 0;
    _hello_body_len = 0;

    start_async_write ();
    start_async_read ();
}

void zlink::asio_ws_engine_t::start_async_read ()
{
    if (_read_pending || _input_stopped || _io_error || !_ws_handshake_complete)
        return;

    WS_ENGINE_DBG ("start_async_read");

    _read_pending = true;

    //  Determine read target buffer
    unsigned char *buffer;
    size_t buffer_size;

    if (_handshaking) {
        if (!_hello_received) {
            if (_hello_header_bytes < zmp_header_size)
                buffer_size = zmp_header_size - _hello_header_bytes;
            else
                buffer_size = _hello_body_len - _hello_body_bytes;
            buffer_size = std::min (buffer_size, _read_buffer.size ());
            buffer = _read_buffer.data ();
        } else {
            buffer = _read_buffer.data ();
            buffer_size = _read_buffer.size ();
        }
    } else if (_decoder) {
        const bool had_partial_prefix = _insize > 0;
        prime_stream_decoder_read_target ();
        _decoder->get_buffer (&_read_buffer_ptr, &buffer_size);
        if (_insize > 0 && _inpos != _read_buffer_ptr) {
            memmove (_read_buffer_ptr, _inpos, _insize);
            _inpos = _read_buffer_ptr;
        }
        if (_insize > 0 && buffer_size > _insize) {
            _read_buffer_ptr += _insize;
            buffer_size -= _insize;
        }
        _last_read_had_partial_prefix = had_partial_prefix;
        _last_read_request_size = buffer_size;
        buffer = _read_buffer_ptr;
    } else {
        //  Use internal buffer
        buffer = _read_buffer.data ();
        buffer_size = _read_buffer.size ();
    }

    const std::weak_ptr<callback_guard_t> callback_guard = _callback_guard;
    _transport->async_read_some (
      buffer, buffer_size,
      [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes_transferred) {
          if (callback_guard.expired ())
              return;
          on_read_complete (ec, bytes_transferred);
      });
}

void zlink::asio_ws_engine_t::on_read_complete (const boost::system::error_code &ec,
                                                std::size_t bytes_transferred)
{
    _read_pending = false;

    WS_ENGINE_DBG ("on_read_complete: ec=%s, bytes=%zu", ec.message ().c_str (), bytes_transferred);

    if (_terminating)
        return;

    if (ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        WS_ENGINE_DBG ("Read error");
        error (connection_error);
        return;
    }

    if (bytes_transferred == 0) {
        WS_ENGINE_DBG ("Connection closed by peer");
        error (connection_error);
        return;
    }

    if (_handshaking) {
        _inpos = _read_buffer.data ();
        _insize = bytes_transferred;
        _input_in_decoder_buffer = false;
        if (!handshake ()) {
            //  Handshake not complete, continue reading
            start_async_read ();
            return;
        }
        //  Handshake complete, proceed to normal operation
        _handshaking = false;

        //  Cancel handshake timer
        if (_has_handshake_timer) {
            cancel_handshake_timer ();
        }

        _next_msg = &asio_ws_engine_t::pull_msg_from_session;
        _process_msg = &asio_ws_engine_t::decode_and_push;

        if (_encoder == NULL) {
            _encoder = new (std::nothrow) zmp_encoder_t (_options.out_batch_size);
            alloc_assert (_encoder);
        }
        if (_decoder == NULL) {
            _decoder =
              new (std::nothrow) zmp_decoder_t (_options.in_batch_size, _options.maxmsgsize);
            alloc_assert (_decoder);
        }

        _last_error_code = 0;
        _last_error_reason.clear ();

        //  Notify session that engine is ready
        if (_session) {
            _session->set_peer_routing_id (_peer_routing_id, _peer_routing_id_size);
            _session->engine_ready ();
        }

        if (_options.recv_routing_id && _session) {
            msg_t routing_id;
            const int rc = routing_id.init_size (_peer_routing_id_size);
            errno_assert (rc == 0);
            if (_peer_routing_id_size > 0)
                memcpy (routing_id.data (), _peer_routing_id, _peer_routing_id_size);
            routing_id.set_flags (msg_t::routing_id);
            const int push_rc = _session->push_msg (&routing_id);
            if (push_rc == 0) {
                _session->flush ();
            } else {
                //  Align with zmp engine behavior: session pipe can be gone
                //  during shutdown races.
                errno_assert (errno == EAGAIN);
            }
        }

        //  Notify the socket only after both transport lanes are ready.
        if (_socket && !paired_transport ()) {
            _socket->event_connection_ready_changed (_endpoint_uri_pair, _peer_routing_id,
                                                     _peer_routing_id_size);
        } else if (_socket) {
            _socket->event_transport_pair_lane_ready (
              _endpoint_uri_pair, _peer_routing_id, _peer_routing_id_size,
              _negotiated_transport_lane, _negotiated_transport_pair_id,
              _negotiated_transport_pair_generation);
        }

        //  Trigger output to start sending any pending messages
        if (!_output_stopped && !_write_pending) {
            start_async_write ();
        }
    } else {
        //  Normal operation: process received data
        maybe_grow_stream_decoder_read_target (bytes_transferred);

        //  Data was read directly into decoder buffer via get_buffer()
        if (_decoder && _insize > 0) {
            const size_t partial_size = _insize;
            _insize = partial_size + bytes_transferred;
        } else {
            _inpos = _read_buffer_ptr;
            _insize = bytes_transferred;
        }
        _input_in_decoder_buffer = true;

        WS_ENGINE_DBG ("on_read_complete: calling process_input, inpos=%p, insize=%zu",
                       static_cast<void *> (_inpos), _insize);

        if (!process_input ()) {
            return;
        }
    }

    start_async_read ();
}

void zlink::asio_ws_engine_t::start_async_write ()
{
    if (_write_pending || _output_stopped || _io_error || !_ws_handshake_complete)
        return;

    WS_ENGINE_DBG ("start_async_write: outsize=%zu", _outsize);

    if (_outsize == 0 || _outpos == NULL) {
        //  No data prepared, try gather path first, then fallback to encoder.
        if (prepare_gather_output ())
            return;
        if (!prepare_output_buffer ()) {
            WS_ENGINE_DBG ("No data to write");
            _output_stopped = true;
            return;
        }
    }

    WS_ENGINE_DBG ("start_async_write: sending %zu bytes", _outsize);

    _write_pending = true;

    const std::weak_ptr<callback_guard_t> callback_guard = _callback_guard;
    _transport->async_write_some (
      _outpos, _outsize,
      [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes_transferred) {
          if (callback_guard.expired ())
              return;
          on_write_complete (ec, bytes_transferred);
      });
}

void zlink::asio_ws_engine_t::on_write_complete (const boost::system::error_code &ec,
                                                 std::size_t bytes_transferred)
{
    _write_pending = false;

    WS_ENGINE_DBG ("on_write_complete: ec=%s, bytes=%zu", ec.message ().c_str (),
                   bytes_transferred);

    if (_terminating)
        return;

    if (ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        WS_ENGINE_DBG ("Write error");
        finish_gather_output ();
        error (connection_error);
        return;
    }

    if (_async_gather)
        finish_gather_output ();

    //  Clear output buffer - async write is complete
    _outpos = NULL;
    _outsize = 0;

    if (prepare_deferred_ready_reply ()) {
        start_async_write ();
        return;
    }

    //  If still handshaking and there's nothing more to write, just return
    if (_handshaking && _outsize == 0)
        return;

    //  Continue writing if more data available
    if (!_output_stopped)
        start_async_write ();
}

bool zlink::asio_ws_engine_t::handshake ()
{
    return handshake_zmp ();
}

bool zlink::asio_ws_engine_t::handshake_zmp ()
{
    if (!_hello_received) {
        if (!receive_hello ()) {
            errno = EAGAIN;
            return false;
        }
    }

    if (!_hello_sent)
        return false;

    if (_decoder == NULL) {
        _decoder = new (std::nothrow) zmp_decoder_t (_options.in_batch_size, _options.maxmsgsize);
        alloc_assert (_decoder);
        _input_in_decoder_buffer = false;
    }

    if (!process_zmp_handshake_input ())
        return false;

    return _ready_received;
}

bool zlink::asio_ws_engine_t::receive_hello ()
{
    while (_insize > 0) {
        const zmp_control::hello_receive_result_t result =
          zmp_control::receive_hello_bytes (_inpos, _insize, _hello_recv, _hello_header_bytes,
                                            _hello_body_bytes, _hello_body_len);
        if (result.status == zmp_control::hello_receive_incomplete)
            return false;
        if (result.status == zmp_control::hello_receive_error) {
            set_last_error (result.error_code, result.error_reason);
            error (protocol_error);
            return false;
        }

        if (parse_hello (_hello_recv, zmp_header_size + _hello_body_len)) {
            _hello_received = true;
            return true;
        }

        return false;
    }

    return false;
}

bool zlink::asio_ws_engine_t::parse_hello (const unsigned char *data_, size_t size_)
{
    zmp_control::hello_parse_result_t result;
    if (zmp_control::parse_hello_frame (data_, size_, _options.type, &result) != 0) {
        set_last_error (result.error_code, result.error_reason);
        if (result.malformed_hello_event) {
            _socket->event_handshake_failed_protocol (
              _session->get_endpoint (), ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_HELLO);
        }
        error (protocol_error);
        return false;
    }

    _peer_routing_id_size = result.identity_len;
    if (result.identity_len > 0)
        memcpy (_peer_routing_id, result.identity, result.identity_len);

    return true;
}

bool zlink::asio_ws_engine_t::process_zmp_handshake_input ()
{
    if (_decoder == NULL)
        return true;

    const bool input_in_decoder_buffer = _input_in_decoder_buffer;
    int rc = 0;
    size_t processed = 0;

    while (_insize > 0) {
        unsigned char *decode_buf = _inpos;
        size_t decode_size = _insize;

        if (!input_in_decoder_buffer) {
            _decoder->get_buffer (&decode_buf, &decode_size);
            decode_size = std::min (_insize, decode_size);
            memcpy (decode_buf, _inpos, decode_size);
            _decoder->resize_buffer (decode_size);
        }

        rc = _decoder->decode (decode_buf, decode_size, processed);
        _inpos += processed;
        _insize -= processed;

        if (rc == 0 || rc == -1)
            break;

        msg_t *msg = _decoder->msg ();
        const unsigned char msg_flags = msg->flags ();
        if (msg_flags & msg_t::command) {
            rc = process_command_message (msg);
        } else {
            set_last_error (zmp_error_internal, "data before ready");
            errno = EPROTO;
            rc = -1;
        }

        if (rc == -1)
            break;

        if (_ready_received)
            break;
    }

    if (rc == -1) {
        if (errno != EAGAIN)
            error (protocol_error);
        return false;
    }

    return true;
}

int zlink::asio_ws_engine_t::process_ready_message (msg_t *msg_)
{
    properties_t properties;
    init_properties (properties);
    const char *error_reason = NULL;
    if (zmp_control::accept_ready_message (msg_, _ready_received, _metadata, properties,
                                           &error_reason)
        != 0) {
        set_last_error (zmp_error_internal, error_reason);
        return -1;
    }

    uint64_t peer_max_message_bytes = 0;
    if (zmp_metadata::parse_max_message_size (properties, &peer_max_message_bytes) < 0) {
        set_last_error (zmp_error_internal, "maximum message size metadata invalid");
        return -1;
    }
    _session->set_peer_max_message_bytes (peer_max_message_bytes);

    transport_lane_t lane = transport_lane_application;
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    const int pair_rc =
      zmp_metadata::parse_transport_pair (properties, &lane, &pair_id, &generation);
    if (pair_rc < 0 || (paired_transport () && pair_rc == 0)
        || (!paired_transport () && pair_rc != 0)
        || (pair_rc > 0
            && _session->set_peer_transport_pair (lane, pair_id, generation) != 0)) {
        set_last_error (zmp_error_internal, "transport pair metadata invalid");
        return -1;
    }
    if (paired_transport ()) {
        _negotiated_transport_lane = lane;
        _negotiated_transport_pair_id = pair_id;
        _negotiated_transport_pair_generation = generation;
    }
    if (paired_transport () && !_options.transport_pair_initiator)
        schedule_ready_reply (lane, pair_id, generation);
    return 0;
}

bool zlink::asio_ws_engine_t::paired_transport () const
{
    return _options.type == ZLINK_CORE_SOCKET_DEALER
           || _options.type == ZLINK_CORE_SOCKET_ROUTER;
}

void zlink::asio_ws_engine_t::schedule_ready_reply (
  transport_lane_t lane_, uint64_t pair_id_, uint64_t generation_)
{
    options_t response_options = _options;
    response_options.zmp_metadata = true;
    response_options.transport_lane = lane_;
    response_options.transport_pair_id = pair_id_;
    response_options.transport_pair_generation = generation_;
    zmp_control::build_ready_frame (response_options, _deferred_ready_send);
    _deferred_ready_pending = true;
    if (_outsize == 0 && prepare_deferred_ready_reply ())
        start_async_write ();
}

bool zlink::asio_ws_engine_t::prepare_deferred_ready_reply ()
{
    if (!_deferred_ready_pending || _outsize != 0)
        return false;
    _outpos = &_deferred_ready_send[0];
    _outsize = _deferred_ready_send.size ();
    _deferred_ready_pending = false;
    _ready_sent = true;
    return true;
}

int zlink::asio_ws_engine_t::process_error_message (msg_t *msg_)
{
    uint8_t code = zmp_error_internal;
    const char *error_reason = NULL;
    zmp_control::parse_error_frame (msg_, &code, &error_reason);
    set_last_error (code, error_reason);
    errno = EPROTO;
    return -1;
}

bool zlink::asio_ws_engine_t::process_input ()
{
    WS_ENGINE_DBG ("process_input: insize=%zu, inpos=%p, in_decoder_buffer=%d", _insize,
                   static_cast<void *> (_inpos), _input_in_decoder_buffer);

    if (!_decoder)
        return true;

    const bool input_in_decoder_buffer = _input_in_decoder_buffer;

    int rc = 0;
    size_t processed = 0;

    while (_insize > 0) {
        unsigned char *decode_buf;
        size_t decode_size;

        decode_buf = _inpos;
        decode_size = _insize;

        if (!input_in_decoder_buffer) {
            //  Data came from external buffer (e.g., greeting); copy into
            //  decoder buffer to preserve decoder buffer invariants.
            _decoder->get_buffer (&decode_buf, &decode_size);
            decode_size = std::min (_insize, decode_size);
            memcpy (decode_buf, _inpos, decode_size);
            _decoder->resize_buffer (decode_size);
            WS_ENGINE_DBG ("process_input: copied %zu bytes from external buffer to decoder",
                           decode_size);
        }

        WS_ENGINE_DBG ("process_input: calling decode with size=%zu", decode_size);
        rc = _decoder->decode (decode_buf, decode_size, processed);
        WS_ENGINE_DBG ("process_input: decode rc=%d, processed=%zu", rc, processed);

        zlink_assert (processed <= decode_size);
        _inpos += processed;
        _insize -= processed;

        if (rc == 0 || rc == -1)
            break;

        //  Message decoded, push to session
        msg_t *msg = _decoder->msg ();
        WS_ENGINE_DBG ("process_input: got message, size=%zu, flags=0x%x", msg->size (),
                       msg->flags ());
        rc = (this->*_process_msg) (msg);
        WS_ENGINE_DBG ("process_input: process_msg rc=%d", rc);
        if (rc == -1)
            break;
    }

    //  Tear down connection on decode/process error
    if (rc == -1) {
        if (errno != EAGAIN) {
            WS_ENGINE_DBG ("process_input: decode/process error, errno=%d", errno);
            zmp_decoder_t *decoder = dynamic_cast<zmp_decoder_t *> (_decoder);
            if (decoder && decoder->error_code () != 0)
                set_last_error (decoder->error_code (), NULL);
            error (protocol_error);
            return false;
        }
        _input_stopped = true;
    }

    //  Flush any messages to the socket
    if (_session) {
        _session->flush ();
    }

    return true;
}

bool zlink::asio_ws_engine_t::use_stream_dynamic_read_growth () const
{
    return zlink::asio_stream_fastpath_policy::can_grow_stream_target (
      _options.type, _decoder, _stream_decoder_read_target_size, _stream_decoder_read_target_max);
}

bool zlink::asio_ws_engine_t::use_stream_dynamic_write_growth () const
{
    return zlink::asio_stream_fastpath_policy::can_grow_stream_target (
      _options.type, _encoder, _stream_encoder_write_target_size, _stream_encoder_write_target_max);
}

void zlink::asio_ws_engine_t::prime_stream_decoder_read_target ()
{
    if (_options.type != ZLINK_CORE_SOCKET_STREAM || !_decoder)
        return;

    _decoder->resize_buffer (_stream_decoder_read_target_size);
}

void zlink::asio_ws_engine_t::maybe_grow_stream_decoder_read_target (size_t bytes_transferred_)
{
    const size_t grown = zlink::asio_stream_fastpath_policy::next_decoder_read_target (
      _options.type, _decoder, _stream_decoder_read_target_size, _stream_decoder_read_target_max,
      _last_read_had_partial_prefix, _last_read_request_size, bytes_transferred_,
      &_stream_decoder_read_target_full_hits, 1);
    if (grown == 0)
        return;

    _stream_decoder_read_target_size = grown;
    _decoder->resize_buffer (_stream_decoder_read_target_size);
}

void zlink::asio_ws_engine_t::apply_pending_stream_encoder_resize ()
{
    if (_options.type != ZLINK_CORE_SOCKET_STREAM || !_encoder)
        return;

    if (_stream_encoder_pending_resize_size <= _stream_encoder_write_target_size)
        return;

    _stream_encoder_write_target_size = _stream_encoder_pending_resize_size;
    _stream_encoder_pending_resize_size = 0;
    _encoder->resize_buffer (_stream_encoder_write_target_size);
}

void zlink::asio_ws_engine_t::maybe_schedule_stream_encoder_growth (size_t filled_out_batch_)
{
    const size_t grown = zlink::asio_stream_fastpath_policy::next_encoder_write_target (
      _options.type, _encoder, _stream_encoder_write_target_size, _stream_encoder_write_target_max,
      filled_out_batch_,
      &_stream_encoder_write_target_full_hits, 1);
    if (grown == 0)
        return;

    _stream_encoder_pending_resize_size = grown;
}

bool zlink::asio_ws_engine_t::build_gather_header (const msg_t &msg_,
                                                   unsigned char *buffer_,
                                                   size_t buffer_size_,
                                                   size_t &header_size_)
{
    if (buffer_size_ < zmp_header_size)
        return false;

    const size_t size = msg_.size ();
    const unsigned char msg_flags = msg_.flags ();

    unsigned char flags = 0;
    if (msg_flags != 0) {
        if (msg_flags & msg_t::more)
            flags |= zmp_flag_more;
        if (msg_flags & msg_t::command)
            flags |= zmp_flag_control;
        if (msg_flags & msg_t::routing_id)
            flags |= zmp_flag_identity;

        const unsigned char cmd_type = msg_flags & CMD_TYPE_MASK;
        if (cmd_type == msg_t::subscribe)
            flags |= zmp_flag_subscribe;
        else if (cmd_type == msg_t::cancel)
            flags |= zmp_flag_cancel;
    }

    buffer_[0] = zmp_magic;
    buffer_[1] = zmp_version;
    buffer_[2] = flags;
    buffer_[3] = 0;
    put_uint32 (buffer_ + 4, static_cast<uint32_t> (size));

    header_size_ = zmp_header_size;
    return true;
}

bool zlink::asio_ws_engine_t::prepare_gather_output ()
{
    const bool stream_mode = _options.type == ZLINK_CORE_SOCKET_STREAM;
    const bool gather_enabled = ws_gather_write_on || (stream_mode && ws_stream_gather_on);
    if (!gather_enabled)
        return false;
    if (!_transport || !_transport->supports_gather_write ())
        return false;
    if (_encoder == NULL || _handshaking)
        return false;

    //  Ensure encoder has no in-progress message before loading a new one.
    unsigned char *pending_buf = NULL;
    const size_t pending = _encoder->encode (&pending_buf, 0);
    if (pending > 0) {
        _outpos = pending_buf;
        _outsize = pending;
        return false;
    }

    if ((this->*_next_msg) (&_tx_msg) == -1) {
        if (errno == EAGAIN) {
            _output_stopped = true;
            return true;
        }
        return false;
    }

    const size_t body_size = _tx_msg.size ();
    const size_t threshold = stream_mode ? ws_stream_gather_threshold : ws_gather_threshold;
    if (body_size < threshold) {
        _encoder->load_msg (&_tx_msg);
        return false;
    }

    size_t header_size = 0;
    if (!build_gather_header (_tx_msg, _gather_header, sizeof (_gather_header), header_size)) {
        _encoder->load_msg (&_tx_msg);
        return false;
    }

    _gather_header_size = header_size;
    _gather_body = static_cast<const unsigned char *> (_tx_msg.data ());
    _gather_body_size = body_size;
    _async_gather = true;
    _write_pending = true;
    _output_stopped = false;

    const std::weak_ptr<callback_guard_t> callback_guard = _callback_guard;
    _transport->async_writev (
      _gather_header, _gather_header_size, _gather_body, _gather_body_size,
      [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
          if (callback_guard.expired ())
              return;
          on_write_complete (ec, bytes);
      });
    return true;
}

void zlink::asio_ws_engine_t::finish_gather_output ()
{
    if (!_async_gather)
        return;

    _async_gather = false;
    _gather_header_size = 0;
    _gather_body = NULL;
    _gather_body_size = 0;

    const int rc = _tx_msg.close ();
    errno_assert (rc == 0);
    const int rc_init = _tx_msg.init ();
    errno_assert (rc_init == 0);
}

bool zlink::asio_ws_engine_t::prepare_output_buffer ()
{
    WS_ENGINE_DBG ("prepare_output_buffer: outsize=%zu", _outsize);

    //  If we already have data prepared, return true.
    if (_outsize > 0)
        return true;

    if (!_encoder)
        return false;

    apply_pending_stream_encoder_resize ();

    //  Get initial data from encoder
    _outpos = NULL;
    _outsize = _encoder->encode (&_outpos, 0);

    size_t target_out_batch = static_cast<size_t> (_options.out_batch_size);
    if (_options.type == ZLINK_CORE_SOCKET_STREAM
        && _stream_encoder_write_target_size > target_out_batch) {
        target_out_batch = _stream_encoder_write_target_size;
    }

    //  Fill the output buffer up to batch size
    while (_outsize < target_out_batch) {
        if ((this->*_next_msg) (&_tx_msg) == -1) {
            if (errno == ECONNRESET)
                return false;
            //  Note: we don't set _output_stopped here, let caller decide
            break;
        }

        _encoder->load_msg (&_tx_msg);
        unsigned char *bufptr = _outpos + _outsize;
        const size_t n = _encoder->encode (&bufptr, target_out_batch - _outsize);
        zlink_assert (n > 0);
        if (_outpos == NULL)
            _outpos = bufptr;
        _outsize += n;
    }

    maybe_schedule_stream_encoder_growth (_outsize);

    WS_ENGINE_DBG ("prepare_output_buffer: prepared %zu bytes", _outsize);
    return _outsize > 0;
}

void zlink::asio_ws_engine_t::speculative_write ()
{
    WS_ENGINE_DBG ("speculative_write: write_pending=%d, output_stopped=%d", _write_pending,
                   _output_stopped);

    //  Guard: If async write is already in progress, skip.
    //  This ensures single write-in-flight invariant.
    if (_write_pending)
        return;

    //  Guard: Don't write during I/O errors
    if (_io_error)
        return;

    //  Guard: WebSocket handshake must be complete
    if (!_ws_handshake_complete)
        return;

    //  Prepare output buffer from encoder
    if (!prepare_output_buffer ()) {
        _output_stopped = true;
        WS_ENGINE_DBG ("speculative_write: no data to send, output_stopped=true");
        return;
    }

    if (!_transport->supports_speculative_write ()) {
        WS_ENGINE_DBG ("speculative_write: transport prefers async");
        start_async_write ();
        return;
    }

    //  Attempt synchronous write using transport's write_some()
    //  Note: For WebSocket, this writes a complete frame or returns 0 with EAGAIN
    zlink_assert (_transport);
    const std::size_t bytes =
      _transport->write_some (reinterpret_cast<const std::uint8_t *> (_outpos), _outsize);

    WS_ENGINE_DBG ("speculative_write: write_some returned %zu, errno=%d", bytes, errno);

    if (bytes == 0) {
        //  Check if it's would_block (retry later) or actual error
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            //  would_block - fall back to async write.
            WS_ENGINE_DBG ("speculative_write: would_block, falling back to async");
            start_async_write ();
            return;
        }
        //  Actual error
        WS_ENGINE_DBG ("speculative_write: error, errno=%d", errno);
        error (connection_error);
        return;
    }

    //  For WebSocket, frame-based write means we either wrote the entire
    //  frame or got would_block. Update buffer pointers.
    _outpos += bytes;
    _outsize -= bytes;

    WS_ENGINE_DBG ("speculative_write: wrote %zu bytes, remaining=%zu", bytes, _outsize);

    if (_outsize > 0) {
        //  Partial write (shouldn't happen for WebSocket frame-based write,
        //  but handle it for robustness).
        WS_ENGINE_DBG ("speculative_write: partial write, async for remaining %zu", _outsize);
        start_async_write ();
    } else {
        //  Complete write succeeded.
        //  Try to get more data and continue speculative writing.
        _output_stopped = false;

        //  During handshake, don't loop - let handshake control flow proceed.
        if (_handshaking)
            return;

        //  Try to prepare and write more data speculatively.
        while (prepare_output_buffer ()) {
            const std::size_t more_bytes =
              _transport->write_some (reinterpret_cast<const std::uint8_t *> (_outpos), _outsize);

            WS_ENGINE_DBG ("speculative_write loop: wrote %zu, errno=%d", more_bytes, errno);

            if (more_bytes == 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    //  would_block - remaining data via async
                    start_async_write ();
                    return;
                }
                //  Actual error
                error (connection_error);
                return;
            }

            _outpos += more_bytes;
            _outsize -= more_bytes;

            if (_outsize > 0) {
                //  Partial write - async for remaining
                start_async_write ();
                return;
            }
        }

        //  No more data to send
        _output_stopped = true;
        WS_ENGINE_DBG ("speculative_write: all data sent, output_stopped=true");
    }
}

void zlink::asio_ws_engine_t::process_output ()
{
    WS_ENGINE_DBG ("process_output: outsize=%zu", _outsize);

    //  Preserve encoder buffer and avoid extra copy for async write.
    if (!_encoder || _outsize > 0)
        return;

    prepare_output_buffer ();
}

int zlink::asio_ws_engine_t::pull_msg_from_session (msg_t *msg_)
{
    return _session->pull_msg (msg_);
}

int zlink::asio_ws_engine_t::push_msg_to_session (msg_t *msg_)
{
    return _session->push_msg (msg_);
}

void zlink::asio_ws_engine_t::set_last_error (uint8_t code_, const char *reason_)
{
    _last_error_code = code_;
    if (reason_ && *reason_)
        _last_error_reason.assign (reason_);
    else
        _last_error_reason.assign (zmp_error_reason (code_));
}

void zlink::asio_ws_engine_t::send_error_frame (uint8_t code_, const char *reason_)
{
    if (!_transport || !_transport->is_open ())
        return;

    const char *reason = reason_;
    if (!reason || !*reason)
        reason = zmp_error_reason (code_);

    const size_t reason_len = std::min (strlen (reason), static_cast<size_t> (UCHAR_MAX));
    const size_t body_len = 3 + reason_len;
    unsigned char buffer[zmp_header_size + 3 + UCHAR_MAX];

    buffer[0] = zmp_magic;
    buffer[1] = zmp_version;
    buffer[2] = zmp_flag_control;
    buffer[3] = 0;
    put_uint32 (buffer + 4, static_cast<uint32_t> (body_len));
    buffer[zmp_header_size + 0] = zmp_control_error;
    buffer[zmp_header_size + 1] = code_;
    buffer[zmp_header_size + 2] = static_cast<unsigned char> (reason_len);
    if (reason_len > 0)
        memcpy (buffer + zmp_header_size + 3, reason, reason_len);

    const size_t total = zmp_header_size + body_len;
    size_t offset = 0;
    while (offset < total) {
        const std::size_t written = _transport->write_some (buffer + offset, total - offset);
        if (written == 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }
        offset += written;
    }
}

void zlink::asio_ws_engine_t::error (error_reason_t reason_)
{
    WS_ENGINE_DBG ("error: reason=%d", reason_);

    if (_terminating)
        return;

    const bool handshaked = !_handshaking;

    // protocol errors have been signaled at the point where they occurred
    if (reason_ != protocol_error && _handshaking && _socket) {
        const int err = errno;
        _socket->event_handshake_failed_no_detail (_endpoint_uri_pair, err);
    }

    if (reason_ == timeout_error) {
        if (_handshaking)
            set_last_error (zmp_error_handshake_timeout, NULL);
        else if (_last_error_code == 0)
            set_last_error (zmp_error_internal, NULL);
    } else if (reason_ == protocol_error && _last_error_code == 0) {
        zmp_decoder_t *decoder = dynamic_cast<zmp_decoder_t *> (_decoder);
        if (decoder && decoder->error_code () != 0)
            set_last_error (decoder->error_code (), NULL);
        else
            set_last_error (zmp_error_internal, NULL);
    }

    if (reason_ == timeout_error || reason_ == protocol_error) {
        const uint8_t code = _last_error_code ? _last_error_code : zmp_error_internal;
        send_error_frame (code, _last_error_reason.c_str ());
    }

    _io_error = true;

    uint64_t disconnect_reason = ZLINK_DISCONNECT_UNKNOWN;
    if (_socket && _socket->is_ctx_terminated ()) {
        disconnect_reason = ZLINK_DISCONNECT_CTX_TERM;
    } else if (_handshaking || reason_ == protocol_error) {
        disconnect_reason = ZLINK_DISCONNECT_HANDSHAKE_FAILED;
    } else if (reason_ == timeout_error || reason_ == connection_error) {
        disconnect_reason = ZLINK_DISCONNECT_TRANSPORT_ERROR;
    }

    const blob_t *routing_id = _session ? &_session->peer_routing_id () : NULL;
    const unsigned char *routing_id_data = routing_id ? routing_id->data () : NULL;
    const size_t routing_id_size = routing_id ? routing_id->size () : 0;

    if (_socket) {
        _socket->event_disconnected (_endpoint_uri_pair, disconnect_reason, routing_id_data,
                                     routing_id_size);
    }

    if (_session) {
        _session->flush ();
        _session->engine_error (handshaked, reason_);
    }

    terminate ();
}

void zlink::asio_ws_engine_t::terminate ()
{
    WS_ENGINE_DBG ("terminate: read_pending=%d, write_pending=%d", _read_pending, _write_pending);

    if (_terminating)
        return;

    _terminating = true;
    _callback_guard.reset ();

    //  Cancel all timers
    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }

    //  Close transport - this will cause pending async ops to fail
    if (_transport) {
        _transport->close ();
    }

    _plugged = false;
    _session = NULL;
    if (_io_context) {
        schedule_terminate_completion ();
        return;
    }

    destroy_after_callbacks ();
}

void zlink::asio_ws_engine_t::schedule_terminate_completion ()
{
    if (!_io_context) {
        destroy_after_callbacks ();
        return;
    }

    boost::asio::post (*_io_context, [this] () {
        if (_read_pending || _write_pending) {
            schedule_terminate_completion ();
            return;
        }

        destroy_after_callbacks ();
    });
}

void zlink::asio_ws_engine_t::destroy_after_callbacks ()
{
    _callback_guard.reset ();
    zlink::release_heap_owned (this);
}

bool zlink::asio_ws_engine_t::restart_input ()
{
    WS_ENGINE_DBG ("restart_input");

    _input_stopped = false;

    if (!_read_pending) {
        start_async_read ();
    }

    return true;
}

void zlink::asio_ws_engine_t::restart_output ()
{
    WS_ENGINE_DBG ("restart_output: io_error=%d, output_stopped=%d, write_pending=%d", _io_error,
                   _output_stopped, _write_pending);

    if (_io_error)
        return;

    _output_stopped = false;

    //  Use speculative write for immediate transmission.
    //  This tries synchronous write first, falling back to async if needed.
    speculative_write ();
}

const zlink::endpoint_uri_pair_t &zlink::asio_ws_engine_t::get_endpoint () const
{
    return _endpoint_uri_pair;
}

void zlink::asio_ws_engine_t::set_handshake_timer ()
{
    add_timer (_options.handshake_ivl, handshake_timer_id);
    _has_handshake_timer = true;
}

void zlink::asio_ws_engine_t::cancel_handshake_timer ()
{
    cancel_timer (handshake_timer_id);
    _has_handshake_timer = false;
}

void zlink::asio_ws_engine_t::add_timer (int timeout_, int id_)
{
    if (!_timer)
        return;

    _current_timer_id = id_;

    _timer->expires_after (std::chrono::milliseconds (timeout_));
    const std::weak_ptr<callback_guard_t> callback_guard = _callback_guard;
    _timer->async_wait ([this, id_, callback_guard] (const boost::system::error_code &ec) {
        if (callback_guard.expired ())
            return;
        on_timer (id_, ec);
    });
}

void zlink::asio_ws_engine_t::cancel_timer (int id_)
{
    if (!_timer)
        return;

    if (_current_timer_id == id_) {
        _timer->cancel ();
        _current_timer_id = -1;
    }
}

void zlink::asio_ws_engine_t::on_timer (int id_, const boost::system::error_code &ec)
{
    if (_terminating || ec == boost::asio::error::operation_aborted)
        return;

    WS_ENGINE_DBG ("on_timer: id=%d", id_);

    if (id_ == handshake_timer_id) {
        _has_handshake_timer = false;
        error (timeout_error);
    }
}

bool zlink::asio_ws_engine_t::init_properties (properties_t &properties_)
{
    if (_peer_address.empty ())
        return false;

    properties_.ZLINK_MAP_INSERT_OR_EMPLACE (std::string (ZLINK_MSG_PROPERTY_PEER_ADDRESS),
                                             _peer_address);

    return true;
}

int zlink::asio_ws_engine_t::pull_and_encode (msg_t *msg_)
{
    return pull_msg_from_session (msg_);
}

int zlink::asio_ws_engine_t::decode_and_push (msg_t *msg_)
{
    if (msg_->flags () & msg_t::command) {
        const int rc = process_command_message (msg_);
        if (rc < 0)
            return -1;
        if (rc == 0)
            return 0;
    }

    if (!_ready_received) {
        set_last_error (zmp_error_internal, "data before ready");
        errno = EPROTO;
        return -1;
    }

    if ((msg_->flags () & msg_t::routing_id) && !_options.recv_routing_id) {
        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    if (push_msg_to_session (msg_) == -1) {
        if (errno == EAGAIN)
            _process_msg = &asio_ws_engine_t::push_one_then_decode_and_push;
        return -1;
    }
    return 0;
}

int zlink::asio_ws_engine_t::push_one_then_decode_and_push (msg_t *msg_)
{
    return push_msg_to_session (msg_);
}

int zlink::asio_ws_engine_t::process_command_message (msg_t *msg_)
{
    const char *error_reason = NULL;
    switch (zmp_control::classify_command_message (msg_, _ready_received, &error_reason)) {
        case zmp_control::command_message_ready:
            return process_ready_message (msg_);
        case zmp_control::command_message_error:
            return process_error_message (msg_);
        case zmp_control::command_message_data_after_ready:
            return 1;
        case zmp_control::command_message_invalid:
        default:
            set_last_error (zmp_error_internal, error_reason ? error_reason : "invalid control");
            return -1;
    }
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_WS
