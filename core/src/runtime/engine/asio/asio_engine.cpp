/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include "engine/asio/asio_engine.hpp"
#include "engine/asio/asio_stream_fastpath_policy.hpp"
#include "engine/asio/asio_poller.hpp"
#include "engine/asio/i_asio_transport.hpp"
#include "transports/tcp/tcp_transport.hpp"
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"
#include "zlink.h"
#include "utils/config.hpp"
#include "utils/err.hpp"
#include "utils/heap_owner.hpp"
#include "utils/ip.hpp"
#include "transports/tcp/tcp.hpp"
#include "utils/likely.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

#include <cstring>
#include <sstream>

// Debug logging for ASIO engine - enable with -DZLINK_ASIO_DEBUG=1
#if defined(ZLINK_ASIO_DEBUG)
#define ASIO_ENGINE_DEBUG 1
#else
#define ASIO_ENGINE_DEBUG 0
#endif

#if ASIO_ENGINE_DEBUG
#include <cstdio>
#define ENGINE_DBG(fmt, ...) fprintf (stderr, "[ASIO_ENGINE] " fmt "\n", ##__VA_ARGS__)
#else
#define ENGINE_DBG(fmt, ...)
#endif

//  Draft API message property names (kept for internal use)
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
#if defined ZLINK_HAVE_SO_PEERCRED
    else if (family == PF_UNIX) {
        struct ucred cred;
        socklen_t size = sizeof (cred);
        if (!getsockopt (s_, SOL_SOCKET, SO_PEERCRED, &cred, &size)) {
            std::ostringstream buf;
            buf << ":" << cred.uid << ":" << cred.gid << ":" << cred.pid;
            peer_address += buf.str ();
        }
    }
#elif defined ZLINK_HAVE_LOCAL_PEERCRED
    else if (family == PF_UNIX) {
        struct xucred cred;
        socklen_t size = sizeof (cred);
        if (!getsockopt (s_, 0, LOCAL_PEERCRED, &cred, &size)
            && cred.cr_version == XUCRED_VERSION) {
            std::ostringstream buf;
            buf << ":" << cred.cr_uid << ":";
            if (cred.cr_ngroups > 0)
                buf << cred.cr_groups[0];
            buf << ":";
            peer_address += buf.str ();
        }
    }
#endif

    return peer_address;
}

namespace
{
const bool asio_gather_write_on = zlink::asio_stream_fastpath_policy::gather_write_enabled ();

const bool asio_single_write_on = zlink::asio_stream_fastpath_policy::single_write_enabled ();

const size_t asio_gather_threshold = zlink::asio_stream_fastpath_policy::gather_threshold ();

// STREAM sockets are latency/throughput sensitive on small frames.
// Enable gather-write by default for STREAM so we can send
// `len+header` and body with one transport operation without copying
// body into encoder batch buffers.
const bool asio_stream_gather_on = zlink::asio_stream_fastpath_policy::stream_gather_enabled ();

// Keep gather enabled for STREAM once payloads are large enough that copying
// them into the encoder batch is more expensive than emitting header+body
// directly in one transport write.
const size_t asio_stream_gather_threshold =
  zlink::asio_stream_fastpath_policy::stream_gather_threshold ();
const size_t asio_stream_tiny_gather_threshold =
  zlink::asio_stream_fastpath_policy::stream_tiny_gather_threshold ();

const bool asio_trace_on = zlink::asio_stream_fastpath_policy::trace_enabled ();

const bool asio_stream_enable_handler_alloc =
  zlink::asio_stream_fastpath_policy::enable_handler_alloc ();

const bool asio_stream_enable_read_drain = zlink::asio_stream_fastpath_policy::enable_read_drain ();

// STREAM/TCP is dominated by small-frame send overhead in high-connection
// tests. Keep speculative write always enabled in the STREAM fast-path.
const bool asio_stream_enable_speculative_write =
  zlink::asio_stream_fastpath_policy::enable_speculative_write ();

const bool asio_stream_enable_rx_slab = zlink::asio_stream_fastpath_policy::enable_rx_slab ();

const bool asio_stream_enable_non_tcp_spec_read =
  zlink::asio_stream_fastpath_policy::enable_non_tcp_spec_read ();

const size_t asio_stream_spec_write_budget_bytes =
  zlink::asio_stream_fastpath_policy::spec_write_budget_bytes ();

const size_t asio_stream_read_drain_max_loops =
  zlink::asio_stream_fastpath_policy::read_drain_max_loops ();

const size_t asio_stream_read_drain_max_bytes =
  zlink::asio_stream_fastpath_policy::read_drain_max_bytes ();


}

zlink::asio_engine_t::asio_engine_t (fd_t fd_,
                                     const options_t &options_,
                                     const endpoint_uri_pair_t &endpoint_uri_pair_,
                                     std::unique_ptr<i_asio_transport> transport_) :
    _options (options_),
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
    _input_stopped (false),
    _output_stopped (false),
    _endpoint_uri_pair (endpoint_uri_pair_),
    _has_handshake_timer (false),
    _peer_address (get_peer_address (fd_)),
    _has_handshake_stage (true),
    _transport_adapter (),
    _pipeline (),
    _connection_facade ()
{
    ENGINE_DBG ("Constructor called, fd=%d", fd_);

    _transport_adapter.transport = std::move (transport_);
    //  _pipeline.read_buffer is allocated lazily on the first handshake
    //  read (see start_async_read/speculative_read). Raw engines create
    //  their decoder at plug time and never use it.
    _transport_adapter.fd = fd_;
    _connection_facade.callback_guard.reset (new connection_facade_t::callback_guard_t ());
    _pipeline.stream_decoder_read_target_size =
      zlink::asio_stream_fastpath_policy::decoder_initial_read_target (options_);
    _pipeline.stream_decoder_read_target_max =
      zlink::asio_stream_fastpath_policy::decoder_max_read_target (options_);
    _pipeline.stream_encoder_write_target_size =
      zlink::asio_stream_fastpath_policy::encoder_initial_write_target (options_);
    _pipeline.stream_encoder_write_target_max =
      zlink::asio_stream_fastpath_policy::encoder_max_write_target (options_);

    const int rc = _pipeline.tx_msg.init ();
    errno_assert (rc == 0);

    if (!_transport_adapter.transport) {
        _transport_adapter.transport =
          std::unique_ptr<i_asio_transport> (new (std::nothrow) tcp_transport_t ());
        alloc_assert (_transport_adapter.transport);
    }

    //  Put the socket into non-blocking mode.
    unblock_socket (_transport_adapter.fd);
}

zlink::asio_engine_t::~asio_engine_t ()
{
    ENGINE_DBG ("Destructor called");

    zlink_assert (!_connection_facade.plugged);

    if (_transport_adapter.transport) {
        _transport_adapter.transport->close ();
    } else if (_transport_adapter.fd != retired_fd) {
#ifdef ZLINK_HAVE_WINDOWS
        const int rc = closesocket (_transport_adapter.fd);
        wsa_assert (rc != SOCKET_ERROR);
#else
        int rc = close (_transport_adapter.fd);
#if defined(__FreeBSD_kernel__) || defined(__FreeBSD__)
        // FreeBSD may return ECONNRESET on close() under load but this is not
        // an error.
        if (rc == -1 && errno == ECONNRESET)
            rc = 0;
#endif
        errno_assert (rc == 0);
#endif
        _transport_adapter.fd = retired_fd;
    }

    const int rc = _pipeline.tx_msg.close ();
    errno_assert (rc == 0);

    //  Drop reference to metadata and destroy it if we are
    //  the only user.
    if (_metadata != NULL) {
        if (_metadata->drop_ref ()) {
            LIBZLINK_DELETE (_metadata);
        }
    }

    LIBZLINK_DELETE (_encoder);
    LIBZLINK_DELETE (_decoder);

    //  Clear pending buffers (True Proactor Pattern)
    _pipeline.pending_buffers.clear ();
    _pipeline.pending_stream_rx_chunks.clear ();
    _pipeline.total_pending_bytes = 0;

    //  Smart pointers will automatically clean up ASIO objects
    //  (_transport_adapter.timer, _socket_handle/_stream_descriptor)
}

void zlink::asio_engine_t::plug (io_thread_t *io_thread_, session_base_t *session_)
{
    ENGINE_DBG ("plug called");

    zlink_assert (!_connection_facade.plugged);
    _connection_facade.plugged = true;

    //  Connect to session object.
    zlink_assert (!_connection_facade.session);
    zlink_assert (session_);
    _connection_facade.session = session_;
    _connection_facade.socket = _connection_facade.session->get_socket ();

    //  Get reference to io_context from the io_thread's poller
    asio_poller_t *poller = static_cast<asio_poller_t *> (io_thread_->get_poller ());
    _transport_adapter.io_context = &poller->get_io_context ();

    //  Allocate timer with correct io_context
    _transport_adapter.timer = std::unique_ptr<boost::asio::steady_timer> (
      new boost::asio::steady_timer (*_transport_adapter.io_context));

    _pipeline.io_error = false;

    if (!_transport_adapter.transport
        || !_transport_adapter.transport->open (*_transport_adapter.io_context,
                                                _transport_adapter.fd)) {
        error (connection_error);
        return;
    }

    if (_transport_adapter.transport->requires_handshake ()) {
        start_transport_handshake ();
        return;
    }

    plug_internal ();
}

void zlink::asio_engine_t::start_transport_handshake ()
{
    ENGINE_DBG ("start_transport_handshake");

    _pipeline.handshake_pending = true;
    if (_options.handshake_ivl > 0)
        set_handshake_timer ();

    const int handshake_type = _endpoint_uri_pair.local_type == endpoint_type_connect ? 0 : 1;
    const std::weak_ptr<connection_facade_t::callback_guard_t> callback_guard =
      _connection_facade.callback_guard;

    const bool use_stream_handler_alloc =
      _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_handler_alloc;
    if (use_stream_handler_alloc) {
        _transport_adapter.transport->async_handshake (
          handshake_type,
          make_custom_alloc_handler (
            _stream_handshake_handler_allocator,
            [this, callback_guard] (const boost::system::error_code &ec, std::size_t) {
                if (callback_guard.expired ())
                    return;
                on_transport_handshake (ec);
            }));
    } else {
        _transport_adapter.transport->async_handshake (
          handshake_type,
          [this, callback_guard] (const boost::system::error_code &ec, std::size_t) {
              if (callback_guard.expired ())
                  return;
              on_transport_handshake (ec);
          });
    }
}

void zlink::asio_engine_t::on_transport_handshake (const boost::system::error_code &ec)
{
    ENGINE_DBG ("on_transport_handshake: ec=%s, terminating=%d", ec.message ().c_str (),
                _connection_facade.terminating);

    _pipeline.handshake_pending = false;
    if (_connection_facade.terminating)
        return;

    if (!_connection_facade.plugged)
        return;

    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }

    if (ec) {
        error (connection_error);
        return;
    }

    plug_internal ();
}

void zlink::asio_engine_t::complete_handshake ()
{
    if (!_connection_facade.handshaking)
        return;

    _connection_facade.handshaking = false;

    if (_has_handshake_stage) {
        _connection_facade.session->engine_ready ();
        _has_handshake_stage = false;
    }

    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }
}

void zlink::asio_engine_t::unplug ()
{
    ENGINE_DBG ("unplug called");

    zlink_assert (_connection_facade.plugged);
    _connection_facade.plugged = false;

    //  Cancel all timers.
    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }

    //  Cancel pending async operations by closing the transport
    if (_transport_adapter.transport)
        _transport_adapter.transport->close ();
    if (_transport_adapter.timer)
        _transport_adapter.timer->cancel ();

    //  Clear pending buffers (True Proactor Pattern)
    _pipeline.pending_buffers.clear ();
    _pipeline.pending_stream_rx_chunks.clear ();
    _pipeline.total_pending_bytes = 0;

    _connection_facade.session = NULL;
}

void zlink::asio_engine_t::terminate ()
{
    ENGINE_DBG ("terminate called");

    if (_connection_facade.terminating)
        return;

    //  Mark as terminating to prevent callbacks from processing
    _connection_facade.terminating = true;
    _connection_facade.callback_guard.reset ();

    unplug ();

    //  Avoid re-entrant poll() during teardown: SSL/WebSocket callbacks can still
    //  be completing while close() is in progress. Let io_context serialize final
    //  callbacks and delete on its queue.
    destroy_after_callbacks ();
}

bool zlink::asio_engine_t::build_gather_header (const msg_t &msg_,
                                                unsigned char *buffer_,
                                                size_t buffer_size_,
                                                size_t &header_size_)
{
    LIBZLINK_UNUSED (msg_);
    LIBZLINK_UNUSED (buffer_);
    LIBZLINK_UNUSED (buffer_size_);
    header_size_ = 0;
    return false;
}

size_t zlink::asio_engine_t::select_handshake_read_buffer ()
{
    //  During handshake, use the internal buffer (allocated on first use).
    if (_pipeline.read_buffer.empty ())
        _pipeline.read_buffer.resize (handshake_read_buffer_size);
    _pipeline.read_buffer_ptr = _pipeline.read_buffer.data ();
    return _pipeline.read_buffer.size ();
}

void zlink::asio_engine_t::start_async_read ()
{
    //  For non-stream sockets keep classic flow-control semantics:
    //  once input is stopped, do not arm new reads until restart_input().
    //  Stream sockets keep proactor-style buffering under backpressure.
    if (_pipeline.read_pending || _pipeline.io_error
        || (_input_stopped && _options.type != ZLINK_CORE_SOCKET_STREAM))
        return;

    ENGINE_DBG ("start_async_read: insize=%zu", _insize);

    _pipeline.read_pending = true;
    _pipeline.read_from_pending_pool = false;
    _pipeline.last_read_request_size = 0;
    _pipeline.last_read_had_partial_prefix = false;

    //  Get buffer from decoder if available
    size_t read_size;

    if (_decoder && _input_stopped && _options.type == ZLINK_CORE_SOCKET_STREAM) {
        read_size = _pipeline.stream_decoder_read_target_size;
        if (read_size == 0) {
            read_size = _options.in_batch_size;
        }
        if (read_size == 0)
            read_size = read_buffer_size;

        if (use_stream_rx_slab () && !_pipeline.pending_stream_rx_chunk_pool.empty ()) {
            asio_stream_rx_chunk_t chunk = ZLINK_MOVE (_pipeline.pending_stream_rx_chunk_pool.back ());
            _pipeline.pending_stream_rx_chunk_pool.pop_back ();
            _pipeline.pending_read_buffer = ZLINK_MOVE (chunk.data);
        } else if (!_pipeline.pending_buffer_pool.empty ()) {
            _pipeline.pending_read_buffer = std::move (_pipeline.pending_buffer_pool.back ());
            _pipeline.pending_buffer_pool.pop_back ();
        }
        _pipeline.pending_read_buffer.resize (read_size);
        _pipeline.read_buffer_ptr = _pipeline.pending_read_buffer.data ();
        _pipeline.read_from_pending_pool = true;
    } else if (_decoder) {
        const bool had_partial_prefix = _insize > 0;
        prime_stream_decoder_read_target ();
        _decoder->get_buffer (&_pipeline.read_buffer_ptr, &read_size);

        //  If we have partial data from previous read, move it to buffer start.
        //  The decoder expects data to start from the beginning of its buffer.
        if (_insize > 0 && _inpos != _pipeline.read_buffer_ptr) {
            ENGINE_DBG ("start_async_read: moving %zu partial bytes to buffer start", _insize);
            memmove (_pipeline.read_buffer_ptr, _inpos, _insize);
            _inpos = _pipeline.read_buffer_ptr;
        }

        //  Read into buffer after any existing partial data
        if (_insize > 0 && read_size > _insize) {
            _pipeline.read_buffer_ptr += _insize;
            read_size -= _insize;
        }
        _pipeline.last_read_had_partial_prefix = had_partial_prefix;
    } else {
        read_size = select_handshake_read_buffer ();
    }

    _pipeline.last_read_request_size = read_size;

    ENGINE_DBG ("start_async_read: reading up to %zu bytes", read_size);
    if (_transport_adapter.transport) {
        const std::weak_ptr<connection_facade_t::callback_guard_t> callback_guard =
          _connection_facade.callback_guard;
        const bool use_stream_handler_alloc =
          _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_handler_alloc;
        if (use_stream_handler_alloc) {
            _transport_adapter.transport->async_read_some (
              _pipeline.read_buffer_ptr, read_size,
              make_custom_alloc_handler (
                _stream_read_handler_allocator,
                [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
                    if (callback_guard.expired ())
                        return;
                    on_read_complete (ec, bytes);
                }));
        } else {
            _transport_adapter.transport->async_read_some (
              _pipeline.read_buffer_ptr, read_size,
              [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
                  if (callback_guard.expired ())
                      return;
                  on_read_complete (ec, bytes);
              });
        }
    }
}

bool zlink::asio_engine_t::speculative_read ()
{
    const bool supports_speculative_read =
      _transport_adapter.transport
      && (_transport_adapter.transport->supports_speculative_read ()
          || use_non_tcp_speculative_read ());
    if (_pipeline.read_pending || _pipeline.io_error || !supports_speculative_read)
        return false;

    _pipeline.last_speculative_read_bytes = 0;
    _pipeline.last_read_request_size = 0;
    _pipeline.last_read_had_partial_prefix = false;

    //  Prepare read buffer the same way as start_async_read().
    size_t read_size;

    if (_decoder) {
        const bool had_partial_prefix = _insize > 0;
        prime_stream_decoder_read_target ();
        _decoder->get_buffer (&_pipeline.read_buffer_ptr, &read_size);

        //  If we have partial data from previous read, move it to buffer start.
        if (_insize > 0 && _inpos != _pipeline.read_buffer_ptr) {
            ENGINE_DBG ("speculative_read: moving %zu partial bytes to buffer start", _insize);
            memmove (_pipeline.read_buffer_ptr, _inpos, _insize);
            _inpos = _pipeline.read_buffer_ptr;
        }

        if (_insize > 0 && read_size > _insize) {
            _pipeline.read_buffer_ptr += _insize;
            read_size -= _insize;
        }
        _pipeline.last_read_had_partial_prefix = had_partial_prefix;
    } else {
        read_size = select_handshake_read_buffer ();
    }

    if (read_size == 0)
        return false;

    _pipeline.last_read_request_size = read_size;

    errno = 0;
    const std::size_t bytes = _transport_adapter.transport->read_some (
      reinterpret_cast<std::uint8_t *> (_pipeline.read_buffer_ptr), read_size);

    if (bytes == 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return false;
        //  Treat EOF or other errors as connection error.
        error (connection_error);
        return true;
    }

    _pipeline.last_speculative_read_bytes = bytes;

    //  Mirror async path behavior for backpressure buffering.
    if (_input_stopped) {
        (void) buffer_stream_backpressure_read (bytes);
        return true;
    }

    maybe_grow_stream_decoder_read_target (bytes);

    if (_decoder && _insize > 0) {
        const size_t partial_size = _insize;
        _insize = partial_size + bytes;
    } else {
        _inpos = _pipeline.read_buffer_ptr;
        _insize = bytes;
    }
    _input_in_decoder_buffer = (_decoder != NULL);

    if (!process_input ()) {
        if (_connection_facade.handshaking && errno == EAGAIN) {
            if (_outsize > 0)
                start_async_write ();
        }
    }

    return true;
}

bool zlink::asio_engine_t::buffer_stream_backpressure_read (size_t bytes_transferred_)
{
    const size_t total_pending = _pipeline.total_pending_bytes + _insize;
    if (total_pending + bytes_transferred_ > max_pending_buffer_size) {
        _pipeline.read_from_pending_pool = false;
        ENGINE_DBG ("buffer_stream_backpressure_read: pending buffer overflow (%zu + %zu > %zu)",
                    total_pending, bytes_transferred_, max_pending_buffer_size);
        error (connection_error);
        return false;
    }

    if (_pipeline.read_from_pending_pool) {
        _pipeline.pending_read_buffer.resize (bytes_transferred_);
        if (use_stream_rx_slab ()) {
            asio_stream_rx_chunk_t chunk;
            chunk.data.swap (_pipeline.pending_read_buffer);
            chunk.offset = 0;
            _pipeline.pending_stream_rx_chunks.push_back (ZLINK_MOVE (chunk));
        } else {
            _pipeline.pending_buffers.push_back (std::move (_pipeline.pending_read_buffer));
        }
        _pipeline.read_from_pending_pool = false;
    } else if (use_stream_rx_slab ()) {
        asio_stream_rx_chunk_t chunk;
        if (!_pipeline.pending_stream_rx_chunk_pool.empty ()) {
            chunk = ZLINK_MOVE (_pipeline.pending_stream_rx_chunk_pool.back ());
            _pipeline.pending_stream_rx_chunk_pool.pop_back ();
        }
        chunk.offset = 0;
        chunk.data.resize (bytes_transferred_);
        std::memcpy (chunk.data.data (), _pipeline.read_buffer_ptr, bytes_transferred_);
        _pipeline.pending_stream_rx_chunks.push_back (ZLINK_MOVE (chunk));
    } else {
        std::vector<unsigned char> buffer;
        if (!_pipeline.pending_buffer_pool.empty ()) {
            buffer = ZLINK_MOVE (_pipeline.pending_buffer_pool.back ());
            _pipeline.pending_buffer_pool.pop_back ();
        }
        buffer.resize (bytes_transferred_);
        std::memcpy (buffer.data (), _pipeline.read_buffer_ptr, bytes_transferred_);
        _pipeline.pending_buffers.push_back (std::move (buffer));
    }

    _pipeline.total_pending_bytes += bytes_transferred_;
    return true;
}

void zlink::asio_engine_t::start_async_write ()
{
    if (_connection_facade.terminating || _pipeline.write_pending || _pipeline.io_error)
        return;

    ENGINE_DBG ("start_async_write: outsize=%zu", _outsize);

    if (_outsize == 0 || _outpos == NULL) {
        //  No data prepared, try gather path first, then fallback to encoder.
        if (prepare_gather_output ())
            return;
        process_output ();
    }

    if (_outsize == 0 || _outpos == NULL) {
        _output_stopped = true;
        return;
    }

    //  Try a synchronous write first when supported (libzlink-like path).
    const bool use_speculative_write = use_stream_speculative_write ();
    if (use_speculative_write) {
        const std::size_t bytes = _transport_adapter.transport->write_some (
          reinterpret_cast<const std::uint8_t *> (_outpos), _outsize);
        if (bytes == 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                error (connection_error);
                return;
            }
        } else {
            _outpos += bytes;
            _outsize -= bytes;
            if (_outsize == 0) {
                // Continue pumping queued pipe data after the synchronous tail
                // write completes. Returning here can strand additional STREAM
                // messages in the pipe until some unrelated event re-arms
                // output, which breaks wakeup after peer reads.
                speculative_write ();
                return;
            }
        }
    }

    _pipeline.write_pending = true;
    _pipeline.async_zero_copy = true;

    if (_transport_adapter.transport) {
        const std::weak_ptr<connection_facade_t::callback_guard_t> callback_guard =
          _connection_facade.callback_guard;
        const bool use_stream_handler_alloc =
          _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_handler_alloc;
        if (use_stream_handler_alloc) {
            _transport_adapter.transport->async_write_some (
              _outpos, _outsize,
              make_custom_alloc_handler (
                _stream_write_handler_allocator,
                [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
                    if (callback_guard.expired ())
                        return;
                    on_write_complete (ec, bytes);
                }));
        } else {
            _transport_adapter.transport->async_write_some (
              _outpos, _outsize,
              [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
                  if (callback_guard.expired ())
                      return;
                  on_write_complete (ec, bytes);
              });
        }
    }
}

bool zlink::asio_engine_t::prepare_gather_output ()
{
    const bool stream_mode = _options.type == ZLINK_CORE_SOCKET_STREAM;
    const bool gather_enabled = asio_gather_write_on || (stream_mode && asio_stream_gather_on);

    if (!gather_enabled)
        return false;
    if (!_transport_adapter.transport || !_transport_adapter.transport->supports_gather_write ())
        return false;
    if (_encoder == NULL || _connection_facade.handshaking)
        return false;

    //  Ensure encoder has no in-progress message before loading a new one.
    unsigned char *pending_buf = NULL;
    const size_t pending = _encoder->encode (&pending_buf, 0);
    if (pending > 0) {
        _outpos = pending_buf;
        _outsize = pending;
        return false;
    }

    if ((this->*_next_msg) (&_pipeline.tx_msg) == -1) {
        if (errno == EAGAIN) {
            _output_stopped = true;
            return true;
        }
        return false;
    }

    const size_t body_size = _pipeline.tx_msg.size ();
    const size_t threshold = stream_mode ? asio_stream_gather_threshold : asio_gather_threshold;
    const bool tiny_stream_gather =
      stream_mode && body_size > 0 && body_size <= asio_stream_tiny_gather_threshold;
    if (!tiny_stream_gather && body_size > 0 && body_size < threshold) {
        _encoder->load_msg (&_pipeline.tx_msg);
        return false;
    }

    size_t header_size = 0;
    if (!build_gather_header (_pipeline.tx_msg, _pipeline.gather_header,
                              sizeof (_pipeline.gather_header), header_size)) {
        _encoder->load_msg (&_pipeline.tx_msg);
        return false;
    }

    _pipeline.gather_header_size = header_size;
    _pipeline.gather_body = static_cast<const unsigned char *> (_pipeline.tx_msg.data ());
    _pipeline.gather_body_size = body_size;
    _pipeline.async_gather = true;
    _pipeline.write_pending = true;
    _pipeline.async_zero_copy = false;
    _output_stopped = false;

    const std::weak_ptr<connection_facade_t::callback_guard_t> callback_guard =
      _connection_facade.callback_guard;
    const bool use_stream_handler_alloc =
      _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_handler_alloc;
    if (use_stream_handler_alloc) {
        _transport_adapter.transport->async_writev (
          _pipeline.gather_header, _pipeline.gather_header_size, _pipeline.gather_body,
          _pipeline.gather_body_size,
          make_custom_alloc_handler (
            _stream_write_handler_allocator,
            [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
                if (callback_guard.expired ())
                    return;
                on_write_complete (ec, bytes);
            }));
    } else {
        _transport_adapter.transport->async_writev (
          _pipeline.gather_header, _pipeline.gather_header_size, _pipeline.gather_body,
          _pipeline.gather_body_size,
          [this, callback_guard] (const boost::system::error_code &ec, std::size_t bytes) {
              if (callback_guard.expired ())
                  return;
              on_write_complete (ec, bytes);
          });
    }
    return true;
}

void zlink::asio_engine_t::finish_gather_output ()
{
    if (!_pipeline.async_gather)
        return;

    _pipeline.async_gather = false;
    _pipeline.gather_header_size = 0;
    _pipeline.gather_body = NULL;
    _pipeline.gather_body_size = 0;

    const int rc = _pipeline.tx_msg.close ();
    errno_assert (rc == 0);
    const int rc_init = _pipeline.tx_msg.init ();
    errno_assert (rc_init == 0);
}

bool zlink::asio_engine_t::is_tcp_transport () const
{
    return _transport_adapter.transport && _transport_adapter.transport->name ()
           && strcmp (_transport_adapter.transport->name (), "tcp") == 0;
}

bool zlink::asio_engine_t::use_stream_speculative_write () const
{
    if (!_transport_adapter.transport)
        return false;

    if (_options.type == ZLINK_CORE_SOCKET_STREAM && is_tcp_transport ())
        return asio_stream_enable_speculative_write;

    return _transport_adapter.transport->supports_speculative_write ();
}

bool zlink::asio_engine_t::use_non_tcp_speculative_read () const
{
    if (!_transport_adapter.transport || _options.type != ZLINK_CORE_SOCKET_STREAM)
        return false;

    if (is_tcp_transport ())
        return true;

    return asio_stream_enable_non_tcp_spec_read;
}

bool zlink::asio_engine_t::use_stream_rx_slab () const
{
    return _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_rx_slab;
}

bool zlink::asio_engine_t::use_stream_dynamic_read_growth () const
{
    return zlink::asio_stream_fastpath_policy::can_grow_stream_target (
      _options.type, _decoder, _pipeline.stream_decoder_read_target_size,
      _pipeline.stream_decoder_read_target_max);
}

bool zlink::asio_engine_t::use_stream_dynamic_write_growth () const
{
    return zlink::asio_stream_fastpath_policy::can_grow_stream_target (
      _options.type, _encoder, _pipeline.stream_encoder_write_target_size,
      _pipeline.stream_encoder_write_target_max);
}

void zlink::asio_engine_t::prime_stream_decoder_read_target ()
{
    if (_options.type != ZLINK_CORE_SOCKET_STREAM || !_decoder)
        return;

    _decoder->resize_buffer (_pipeline.stream_decoder_read_target_size);
}

void zlink::asio_engine_t::maybe_grow_stream_decoder_read_target (size_t bytes_transferred_)
{
    const size_t grown = zlink::asio_stream_fastpath_policy::next_decoder_read_target (
      _options.type, _decoder,
      _pipeline.stream_decoder_read_target_size, _pipeline.stream_decoder_read_target_max,
      _pipeline.last_read_had_partial_prefix, _pipeline.last_read_request_size, bytes_transferred_,
      &_pipeline.stream_decoder_read_target_full_hits, 2);
    if (grown == 0)
        return;

    _pipeline.stream_decoder_read_target_size = grown;
    _decoder->resize_buffer (_pipeline.stream_decoder_read_target_size);
}

void zlink::asio_engine_t::apply_pending_stream_encoder_resize ()
{
    if (_options.type != ZLINK_CORE_SOCKET_STREAM || !_encoder)
        return;

    if (_pipeline.stream_encoder_pending_resize_size <= _pipeline.stream_encoder_write_target_size)
        return;

    _pipeline.stream_encoder_write_target_size = _pipeline.stream_encoder_pending_resize_size;
    _pipeline.stream_encoder_pending_resize_size = 0;
    _encoder->resize_buffer (_pipeline.stream_encoder_write_target_size);
}

void zlink::asio_engine_t::maybe_schedule_stream_encoder_growth (size_t filled_out_batch_)
{
    const size_t grown = zlink::asio_stream_fastpath_policy::next_encoder_write_target (
      _options.type, _encoder,
      _pipeline.stream_encoder_write_target_size, _pipeline.stream_encoder_write_target_max,
      filled_out_batch_, &_pipeline.stream_encoder_write_target_full_hits, 2);
    if (grown == 0)
        return;

    _pipeline.stream_encoder_pending_resize_size = grown;
}

void zlink::asio_engine_t::on_read_complete (const boost::system::error_code &ec,
                                             std::size_t bytes_transferred)
{
    _pipeline.read_pending = false;
    ENGINE_DBG ("on_read_complete: ec=%s, bytes=%zu, terminating=%d, input_stopped=%d",
                ec.message ().c_str (), bytes_transferred, _connection_facade.terminating,
                _input_stopped);

    //  If terminating, just return - terminate() is draining handlers
    if (_connection_facade.terminating)
        return;

    if (!_connection_facade.plugged)
        return;

    if (ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        error (connection_error);
        return;
    }

    if (bytes_transferred == 0) {
        //  Connection closed by peer
        error (connection_error);
        return;
    }

    //  During backpressure, STREAM keeps proactor-style buffering, while
    //  non-stream keeps bytes in decoder buffer and waits for restart_input().
    if (_input_stopped) {
        if (_options.type != ZLINK_CORE_SOCKET_STREAM) {
            if (_decoder && _insize > 0) {
                const size_t partial_size = _insize;
                _insize = partial_size + bytes_transferred;
            } else {
                _inpos = _pipeline.read_buffer_ptr;
                _insize = bytes_transferred;
            }
            _input_in_decoder_buffer = (_decoder != NULL);
            return;
        }

        if (_pipeline.read_from_pending_pool) {
            if (!buffer_stream_backpressure_read (bytes_transferred))
                return;

            ENGINE_DBG ("on_read_complete: buffered %zu bytes (total pending: %zu)",
                        bytes_transferred, _pipeline.total_pending_bytes);

            start_async_read ();
            return;
        }

        if (!buffer_stream_backpressure_read (bytes_transferred))
            return;

        ENGINE_DBG ("on_read_complete: buffered %zu bytes (total pending: %zu)", bytes_transferred,
                    _pipeline.total_pending_bytes);

        //  CRITICAL: Continue async read even during backpressure.
        //  This is the key to True Proactor pattern - we always have a
        //  pending read, so when data arrives it's immediately buffered.
        start_async_read ();
        return;
    }

    maybe_grow_stream_decoder_read_target (bytes_transferred);

    //  Handle buffer pointers based on whether we have partial data
    if (_decoder && _insize > 0) {
        //  We have partial data from previous read.
        //  start_async_read() moved partial data to buffer start and set _inpos there.
        //  New data was read right after the partial data.
        const size_t partial_size = _insize;
        _insize = partial_size + bytes_transferred;
        ENGINE_DBG ("on_read_complete: total %zu bytes (partial=%zu + new=%zu)", _insize,
                    partial_size, bytes_transferred);
        //  Note: Do NOT call resize_buffer() here - it interferes with decoder's
        //  internal state management. The decoder tracks its own buffer state.
    } else {
        //  Fresh read - set buffer pointers
        _inpos = _pipeline.read_buffer_ptr;
        _insize = bytes_transferred;
        //  Note: Do NOT call resize_buffer() here - let process_input() handle
        //  decoder buffer management when data is copied to decoder buffer.
    }
    _input_in_decoder_buffer = (_decoder != NULL);

    //  Process received data
    if (!process_input ()) {
        //  If handshaking and need more data (EAGAIN), continue I/O
        if (_connection_facade.handshaking && errno == EAGAIN) {
            //  Send any output that was built during greeting processing
            if (_outsize > 0)
                start_async_write ();
            start_async_read ();
        }
        return;
    }

    //  Drain a bounded amount of immediately available STREAM data before
    //  re-arming async read. This reduces callback churn on small frames.
    if (_pipeline.in_read_drain)
        return;

    maybe_drain_stream_reads ();
}

void zlink::asio_engine_t::maybe_drain_stream_reads ()
{
    if (_connection_facade.terminating || _pipeline.io_error)
        return;

    const bool supports_speculative_read =
      _transport_adapter.transport
      && (_transport_adapter.transport->supports_speculative_read ()
          || use_non_tcp_speculative_read ());
    const bool should_drain = _options.type == ZLINK_CORE_SOCKET_STREAM
                              && asio_stream_enable_read_drain
                              && asio_stream_read_drain_max_loops > 0 && supports_speculative_read;
    if (!should_drain) {
        start_async_read ();
        return;
    }

    _pipeline.in_read_drain = true;
    size_t drained_loops = 0;
    size_t drained_bytes = 0;

    while (!_connection_facade.terminating && !_pipeline.io_error && !_pipeline.read_pending
           && !_input_stopped && drained_loops < asio_stream_read_drain_max_loops
           && drained_bytes < asio_stream_read_drain_max_bytes) {
        const bool progressed = speculative_read ();
        if (!progressed)
            break;

        ++drained_loops;
        if (_pipeline.last_speculative_read_bytes == 0)
            break;
        drained_bytes += _pipeline.last_speculative_read_bytes;
    }

    _pipeline.in_read_drain = false;

    if (!_connection_facade.terminating && !_pipeline.io_error && !_pipeline.read_pending)
        start_async_read ();
}

void zlink::asio_engine_t::on_write_complete (const boost::system::error_code &ec,
                                              std::size_t bytes_transferred)
{
    _pipeline.write_pending = false;
    ENGINE_DBG ("on_write_complete: ec=%s, bytes=%zu, terminating=%d", ec.message ().c_str (),
                bytes_transferred, _connection_facade.terminating);

    //  If terminating, just return - terminate() is draining handlers
    if (_connection_facade.terminating)
        return;

    if (!_connection_facade.plugged)
        return;

    if (ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        //  IO error - stop writing but continue reading to detect connection close
        _pipeline.io_error = true;
        finish_gather_output ();
        return;
    }

    if (_pipeline.async_gather)
        finish_gather_output ();

    if (_pipeline.async_zero_copy) {
        if (bytes_transferred == 0) {
            error (connection_error);
            return;
        }

        _outpos += bytes_transferred;
        _outsize -= bytes_transferred;

        if (_outsize > 0) {
            _pipeline.write_pending = true;
            if (_transport_adapter.transport) {
                const std::weak_ptr<connection_facade_t::callback_guard_t> callback_guard =
                  _connection_facade.callback_guard;
                const bool use_stream_handler_alloc =
                  _options.type == ZLINK_CORE_SOCKET_STREAM && asio_stream_enable_handler_alloc;
                if (use_stream_handler_alloc) {
                    _transport_adapter.transport->async_write_some (
                      _outpos, _outsize,
                      make_custom_alloc_handler (
                        _stream_write_handler_allocator,
                        [this, callback_guard] (const boost::system::error_code &wec,
                                                std::size_t bytes) {
                            if (callback_guard.expired ())
                                return;
                            on_write_complete (wec, bytes);
                        }));
                } else {
                    _transport_adapter.transport->async_write_some (
                      _outpos, _outsize,
                      [this, callback_guard] (const boost::system::error_code &wec,
                                              std::size_t bytes) {
                          if (callback_guard.expired ())
                              return;
                          on_write_complete (wec, bytes);
                      });
                }
            }
            return;
        }

        _pipeline.async_zero_copy = false;
    }

    //  Some protocols must wait for peer metadata before constructing their
    //  final handshake frame. Let them publish that frame after the current
    //  handshake output has drained.
    if (_outsize == 0 && prepare_deferred_handshake_output ()) {
        start_async_write ();
        return;
    }

    //  If still handshaking and there's nothing more to write, just return.
    if (_connection_facade.handshaking && _outsize == 0)
        return;

    //  Continue writing if more data available.
    //  For STREAM/TCP, prefer speculative loop from completion callback
    //  to reduce async callback churn on small frames.
    if (!_output_stopped) {
        if (_options.type == ZLINK_CORE_SOCKET_STREAM && use_stream_speculative_write ())
            speculative_write ();
        else
            start_async_write ();
    }
}

bool zlink::asio_engine_t::process_input ()
{
    ENGINE_DBG ("process_input: handshaking=%d, insize=%zu", _connection_facade.handshaking,
                _insize);

    //  If still handshaking, receive and process the greeting message.
    if (unlikely (_connection_facade.handshaking)) {
        if (handshake ()) {
            //  Handshaking was successful.
            //  Switch into the normal message flow.
            _connection_facade.handshaking = false;

            if (_has_handshake_stage) {
                _connection_facade.session->engine_ready ();

                if (_has_handshake_timer) {
                    cancel_timer (handshake_timer_id);
                    _has_handshake_timer = false;
                }
            }
        } else
            return false;
    }

    zlink_assert (_decoder);

    //  If there has been an I/O error, stop.
    if (_input_stopped) {
        _pipeline.io_error = true;
        return true;
    }

    const bool input_in_decoder_buffer = _input_in_decoder_buffer;

    ENGINE_DBG ("process_input: after handshake, insize=%zu, input_in_decoder_buffer=%d", _insize,
                input_in_decoder_buffer);

    int rc = 0;
    size_t processed = 0;

    while (_insize > 0) {
        unsigned char *decode_buf;
        size_t decode_size;

        decode_buf = _inpos;
        decode_size = _insize;

        if (!input_in_decoder_buffer) {
            //  Data came from an external buffer (handshake/greeting); copy
            //  into decoder buffer to preserve decoder buffer invariants.
            _decoder->get_buffer (&decode_buf, &decode_size);
            decode_size = std::min (_insize, decode_size);
            memcpy (decode_buf, _inpos, decode_size);
            _decoder->resize_buffer (decode_size);
            ENGINE_DBG ("process_input: copied %zu bytes from external input "
                        "buffer to decoder buffer",
                        decode_size);
        }

        ENGINE_DBG ("process_input: decode loop decode_size=%zu", decode_size);
        rc = _decoder->decode (decode_buf, decode_size, processed);
        ENGINE_DBG ("process_input: decode returned rc=%d, processed=%zu", rc, processed);
        zlink_assert (processed <= decode_size);
        _inpos += processed;
        _insize -= processed;

        if (rc == 0 || rc == -1)
            break;
        rc = (this->*_process_msg) (_decoder->msg ());
        ENGINE_DBG ("process_input: process_msg returned rc=%d", rc);
        if (rc == -1)
            break;
    }

    //  Tear down the connection if we have failed to decode input data
    //  or the session has rejected the message.
    if (rc == -1) {
        if (errno != EAGAIN) {
            error (protocol_error);
            return false;
        }
        _input_stopped = true;
    }

    _connection_facade.session->flush ();
    return true;
}

bool zlink::asio_engine_t::prepare_output_buffer ()
{
    ENGINE_DBG ("prepare_output_buffer: outsize=%zu", _outsize);

    //  If we already have data prepared, return true.
    if (_outsize > 0)
        return true;

    //  Even when we stop as soon as there is no data to send,
    //  there may be a pending async_write.
    if (unlikely (_encoder == NULL)) {
        zlink_assert (_connection_facade.handshaking);
        return false;
    }

    apply_pending_stream_encoder_resize ();

    _outpos = NULL;
    _outsize = _encoder->encode (&_outpos, 0);

    size_t target_out_batch =
      zlink::asio_stream_fastpath_policy::output_target_batch (*this, _options);

    while (_outsize < target_out_batch) {
        if ((this->*_next_msg) (&_pipeline.tx_msg) == -1) {
            if (errno == ECONNRESET)
                return false;
            else
                break;
        }

        _encoder->load_msg (&_pipeline.tx_msg);
        unsigned char *bufptr = _outpos + _outsize;
        const size_t n = _encoder->encode (&bufptr, target_out_batch - _outsize);
        zlink_assert (n > 0);
        if (_outpos == NULL)
            _outpos = bufptr;
        _outsize += n;
    }

    maybe_schedule_stream_encoder_growth (_outsize);

    ENGINE_DBG ("prepare_output_buffer: prepared %zu bytes", _outsize);
    return _outsize > 0;
}

void zlink::asio_engine_t::speculative_write ()
{
    ENGINE_DBG ("speculative_write: write_pending=%d, output_stopped=%d", _pipeline.write_pending,
                _output_stopped);

    //  Guard: If async write is already in progress, skip.
    //  This ensures single write-in-flight invariant.
    if (_pipeline.write_pending)
        return;

    //  Guard: Don't write during I/O errors
    if (_pipeline.io_error)
        return;

    //  Try gather path first for large messages.
    if (prepare_gather_output ())
        return;

    //  Prepare output buffer from encoder
    if (!prepare_output_buffer ()) {
        _output_stopped = true;
        ENGINE_DBG ("speculative_write: no data to send, output_stopped=true");
        return;
    }

    const bool use_speculative_write = use_stream_speculative_write ();
    if (!use_speculative_write) {
        ENGINE_DBG ("speculative_write: transport prefers async");
        start_async_write ();
        return;
    }

    const bool stream_tcp_speculative = _options.type == ZLINK_CORE_SOCKET_STREAM
                                        && is_tcp_transport ()
                                        && asio_stream_enable_speculative_write;
    const size_t stream_spec_budget = asio_stream_spec_write_budget_bytes;
    size_t stream_spec_bytes = 0;

    //  Attempt synchronous write using transport's write_some()
    zlink_assert (_transport_adapter.transport);
    const std::size_t bytes = _transport_adapter.transport->write_some (
      reinterpret_cast<const std::uint8_t *> (_outpos), _outsize);

    ENGINE_DBG ("speculative_write: write_some returned %zu, errno=%d", bytes, errno);

    if (bytes == 0) {
        //  Check if it's would_block (retry later) or actual error
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            //  would_block - fall back to async write.
            //  Copy data to write buffer for async operation (buffer lifetime).
            ENGINE_DBG ("speculative_write: would_block, falling back to async");
            start_async_write ();
            return;
        }
        //  Actual error
        ENGINE_DBG ("speculative_write: error, errno=%d", errno);
        error (connection_error);
        return;
    }

    //  Partial or complete write succeeded
    _outpos += bytes;
    _outsize -= bytes;
    if (stream_tcp_speculative)
        stream_spec_bytes += bytes;

    ENGINE_DBG ("speculative_write: wrote %zu bytes, remaining=%zu", bytes, _outsize);

    if (_outsize > 0) {
        //  Partial write - remaining data needs async path.
        //  Copy remaining data to write buffer for async operation.
        ENGINE_DBG ("speculative_write: partial write, async for remaining %zu", _outsize);
        start_async_write ();
    } else {
        //  Complete write succeeded.
        //  Try to get more data and continue speculative writing.
        _output_stopped = false;

        //  During handshake, don't loop - let handshake control flow proceed.
        if (_connection_facade.handshaking)
            return;

        if (asio_single_write_on) {
            start_async_write ();
            return;
        }

        if (stream_tcp_speculative && stream_spec_budget > 0
            && stream_spec_bytes >= stream_spec_budget) {
            start_async_write ();
            return;
        }

        //  Try to prepare and write more data speculatively.
        //  This loop enables efficient burst writes without async overhead.
        while (prepare_output_buffer ()) {
            const std::size_t more_bytes = _transport_adapter.transport->write_some (
              reinterpret_cast<const std::uint8_t *> (_outpos), _outsize);

            ENGINE_DBG ("speculative_write loop: wrote %zu, errno=%d", more_bytes, errno);

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
            if (stream_tcp_speculative)
                stream_spec_bytes += more_bytes;

            if (_outsize > 0) {
                //  Partial write - async for remaining
                start_async_write ();
                return;
            }
            if (stream_tcp_speculative && stream_spec_budget > 0
                && stream_spec_bytes >= stream_spec_budget) {
                start_async_write ();
                return;
            }
        }

        //  No more data to send
        _output_stopped = true;
        ENGINE_DBG ("speculative_write: all data sent, output_stopped=true");
    }
}

void zlink::asio_engine_t::process_output ()
{
    ENGINE_DBG ("process_output: outsize=%zu", _outsize);

    //  If write buffer is empty, try to read new data from the encoder.
    if (_outsize == 0) {
        //  Even when we stop as soon as there is no data to send,
        //  there may be a pending async_write.
        if (unlikely (_encoder == NULL)) {
            zlink_assert (_connection_facade.handshaking);
            return;
        }

        _outpos = NULL;
        _outsize = _encoder->encode (&_outpos, 0);

        size_t target_out_batch =
          zlink::asio_stream_fastpath_policy::output_target_batch (*this, _options);

        while (_outsize < target_out_batch) {
            if ((this->*_next_msg) (&_pipeline.tx_msg) == -1) {
                if (errno == ECONNRESET)
                    return;
                else
                    break;
            }
            _encoder->load_msg (&_pipeline.tx_msg);
            unsigned char *bufptr = _outpos + _outsize;
            const size_t n = _encoder->encode (&bufptr, target_out_batch - _outsize);
            zlink_assert (n > 0);
            if (_outpos == NULL)
                _outpos = bufptr;
            _outsize += n;
        }

        //  If there is no data to send, mark output as stopped.
        if (_outsize == 0) {
            _output_stopped = true;
            return;
        }
    }
}

void zlink::asio_engine_t::restart_output ()
{
    ENGINE_DBG ("restart_output: io_error=%d, output_stopped=%d, write_pending=%d",
                _pipeline.io_error, _output_stopped, _pipeline.write_pending);

    if (unlikely (_pipeline.io_error))
        return;

    if (likely (_output_stopped)) {
        _output_stopped = false;
    }

    //  Use speculative write for immediate transmission.
    //  This tries synchronous write first, falling back to async if needed.
    speculative_write ();
}

bool zlink::asio_engine_t::restart_input ()
{
    //  write_activated() can be delivered even when input flow is already
    //  running. In that case restart is a no-op.
    if (unlikely (!_input_stopped)) {
        ENGINE_DBG ("restart_input: ignored (input not stopped)");
        return true;
    }

    return restart_input_internal ();
}

bool zlink::asio_engine_t::restart_input_internal ()
{
    zlink_assert (_input_stopped);
    zlink_assert (_connection_facade.session != NULL);
    zlink_assert (_decoder != NULL);

    const size_t pending_queue_size = use_stream_rx_slab ()
                                        ? _pipeline.pending_stream_rx_chunks.size ()
                                        : _pipeline.pending_buffers.size ();
    ENGINE_DBG ("restart_input: pending_buffers=%zu, insize=%zu", pending_queue_size, _insize);

    //  First, try to process the previously failed message
    int rc = (this->*_process_msg) (_decoder->msg ());
    if (rc == -1) {
        if (errno == EAGAIN) {
            //  Still backpressure - stay stopped
            _connection_facade.session->flush ();
            ENGINE_DBG ("restart_input: still backpressure on pending msg");
        } else {
            error (protocol_error);
            return false;
        }
        return true;
    }

    //  Process any remaining data in the current input buffer
    while (_insize > 0) {
        size_t processed = 0;
        unsigned char *decode_buf = _inpos;
        size_t decode_size = _insize;

        if (!_input_in_decoder_buffer) {
            _decoder->get_buffer (&decode_buf, &decode_size);
            decode_size = std::min (_insize, decode_size);
            memcpy (decode_buf, _inpos, decode_size);
            _decoder->resize_buffer (decode_size);
        }

        rc = _decoder->decode (decode_buf, decode_size, processed);
        zlink_assert (processed <= _insize);
        _inpos += processed;
        _insize -= processed;
        if (rc == 0 || rc == -1)
            break;
        rc = (this->*_process_msg) (_decoder->msg ());
        if (rc == -1)
            break;
    }

    //  Check for backpressure or errors after processing current buffer
    if (rc == -1 && errno == EAGAIN) {
        _connection_facade.session->flush ();
        ENGINE_DBG ("restart_input: backpressure after current buffer");
        return true;
    } else if (_pipeline.io_error) {
        error (connection_error);
        return false;
    } else if (rc == -1) {
        error (protocol_error);
        return false;
    }

    //  Process any buffered data from pending queues (if present).
    if (use_stream_rx_slab ()) {
        while (!_pipeline.pending_stream_rx_chunks.empty ()) {
            asio_stream_rx_chunk_t &chunk = _pipeline.pending_stream_rx_chunks.front ();
            const size_t chunk_available = chunk.size ();

            ENGINE_DBG ("restart_input: processing stream slab chunk of %zu bytes",
                        chunk_available);

            unsigned char *decode_buf = NULL;
            size_t decode_size = 0;
            size_t chunk_pos = 0;
            size_t chunk_remaining = chunk_available;

            while (chunk_remaining > 0) {
                size_t processed = 0;
                _decoder->get_buffer (&decode_buf, &decode_size);
                const size_t to_copy = std::min (chunk_remaining, decode_size);
                memcpy (decode_buf, chunk.data.data () + chunk.offset + chunk_pos, to_copy);
                _decoder->resize_buffer (to_copy);

                rc = _decoder->decode (decode_buf, to_copy, processed);
                chunk_pos += processed;
                chunk_remaining -= processed;

                if (rc == 0 || rc == -1)
                    break;

                rc = (this->*_process_msg) (_decoder->msg ());
                if (rc == -1)
                    break;
            }

            if (rc == -1 && errno == EAGAIN) {
                if (chunk_pos > 0) {
                    chunk.offset += chunk_pos;
                    _pipeline.total_pending_bytes -= chunk_pos;
                    ENGINE_DBG ("restart_input: stream slab backpressure, %zu bytes remaining",
                                chunk.size ());
                }
                _connection_facade.session->flush ();
                ENGINE_DBG ("restart_input: backpressure during stream slab chunk");
                return true;
            } else if (_pipeline.io_error) {
                error (connection_error);
                return false;
            } else if (rc == -1) {
                error (protocol_error);
                return false;
            }

            if (rc == 0 && chunk_remaining > 0) {
                if (chunk_pos > 0) {
                    chunk.offset += chunk_pos;
                    _pipeline.total_pending_bytes -= chunk_pos;
                    ENGINE_DBG (
                      "restart_input: stream slab decode needs more data, %zu bytes remaining",
                      chunk.size ());
                }
                break;
            }

            _pipeline.total_pending_bytes -= chunk_available;
            if (_pipeline.pending_stream_rx_chunk_pool.size () < pending_buffer_pool_max) {
                chunk.data.clear ();
                chunk.offset = 0;
                _pipeline.pending_stream_rx_chunk_pool.push_back (ZLINK_MOVE (chunk));
            }
            _pipeline.pending_stream_rx_chunks.pop_front ();
        }
    } else {
        while (!_pipeline.pending_buffers.empty ()) {
            std::vector<unsigned char> &buffer = _pipeline.pending_buffers.front ();
            const size_t original_buffer_size = buffer.size ();

            ENGINE_DBG ("restart_input: processing pending buffer of %zu bytes", buffer.size ());

            //  Copy to decoder buffer and process
            unsigned char *decode_buf;
            size_t decode_size;
            _decoder->get_buffer (&decode_buf, &decode_size);
            decode_size = std::min (buffer.size (), decode_size);
            memcpy (decode_buf, buffer.data (), decode_size);
            _decoder->resize_buffer (decode_size);

            size_t buffer_pos = 0;
            size_t buffer_remaining = buffer.size ();

            while (buffer_remaining > 0) {
                size_t processed = 0;

                //  If we've consumed the initial copy, get more buffer space
                if (buffer_pos > 0) {
                    _decoder->get_buffer (&decode_buf, &decode_size);
                    const size_t to_copy = std::min (buffer_remaining, decode_size);
                    memcpy (decode_buf, buffer.data () + buffer_pos, to_copy);
                    _decoder->resize_buffer (to_copy);
                    decode_size = to_copy;
                } else {
                    //  First iteration uses already copied data
                    decode_size = std::min (buffer_remaining, decode_size);
                }

                rc = _decoder->decode (decode_buf, decode_size, processed);
                buffer_pos += processed;
                buffer_remaining -= processed;

                if (rc == 0 || rc == -1)
                    break;

                rc = (this->*_process_msg) (_decoder->msg ());
                if (rc == -1)
                    break;
            }

            //  If backpressure occurred, keep remaining data in buffer
            if (rc == -1 && errno == EAGAIN) {
                if (buffer_remaining > 0 && buffer_pos > 0) {
                    //  Trim processed data from buffer and update tracking
                    const size_t bytes_consumed = buffer_pos;
                    buffer.erase (buffer.begin (),
                                  buffer.begin () + static_cast<long> (buffer_pos));
                    _pipeline.total_pending_bytes -= bytes_consumed;
                    ENGINE_DBG ("restart_input: partial pending buffer, %zu bytes "
                                "remaining",
                                buffer.size ());
                }
                _connection_facade.session->flush ();
                ENGINE_DBG ("restart_input: backpressure during pending buffer");
                return true;
            } else if (_pipeline.io_error) {
                error (connection_error);
                return false;
            } else if (rc == -1) {
                error (protocol_error);
                return false;
            }

            //  Bug fix: rc == 0 means decoder needs more data but buffer_remaining
            //  may still be > 0. In this case, we must preserve the remaining data.
            if (rc == 0 && buffer_remaining > 0) {
                if (buffer_pos > 0) {
                    //  Trim processed data from buffer and update tracking
                    const size_t bytes_consumed = buffer_pos;
                    buffer.erase (buffer.begin (),
                                  buffer.begin () + static_cast<long> (buffer_pos));
                    _pipeline.total_pending_bytes -= bytes_consumed;
                    ENGINE_DBG ("restart_input: decoder needs more data, %zu bytes "
                                "remaining in buffer",
                                buffer.size ());
                }
                //  Don't pop_front - keep remaining data for next iteration
                //  when more data arrives from network
                break;
            }

            //  Buffer fully processed, remove it and update tracking
            _pipeline.total_pending_bytes -= original_buffer_size;
            if (_pipeline.pending_buffer_pool.size () < pending_buffer_pool_max) {
                buffer.clear ();
                _pipeline.pending_buffer_pool.push_back (std::move (buffer));
            }
            _pipeline.pending_buffers.pop_front ();
        }
    }

    //  All pending data processed successfully - NOW safe to clear flag
    _input_stopped = false;
    _connection_facade.session->flush ();

    ENGINE_DBG ("restart_input: completed, input resumed");

    //  CRITICAL FIX: Double-check for race condition AFTER flush()
    //  Race scenario (discovered by Gemini):
    //  - We cleared _input_stopped = false above
    //  - _connection_facade.session->flush() may trigger pipe events that lead to on_read_complete()
    //  - on_read_complete() sees _input_stopped = false, so it processes data directly
    //  - BUT if it hits backpressure, it won't buffer (since _input_stopped = false)
    //  Solution: Check if any buffers accumulated and re-enter stopped mode
    const size_t pending_after_flush = use_stream_rx_slab ()
                                         ? _pipeline.pending_stream_rx_chunks.size ()
                                         : _pipeline.pending_buffers.size ();
    if (pending_after_flush > 0) {
        ENGINE_DBG ("restart_input: race detected AFTER flush, %zu buffers accumulated, "
                    "re-entering stopped mode",
                    pending_after_flush);
        //  Re-enter stopped mode and recursively drain.
        //  Call restart_input_internal() directly to keep state local.
        _input_stopped = true;
        return restart_input_internal ();
    }

    //  Speculative read (libzlink pattern): drain immediately available data
    //  before re-arming async read. This avoids missed wakeups on IPC.
    if (_options.type == ZLINK_CORE_SOCKET_STREAM && speculative_read ())
        return true;

    start_async_read ();
    return true;
}

const zlink::endpoint_uri_pair_t &zlink::asio_engine_t::get_endpoint () const
{
    return _endpoint_uri_pair;
}

int zlink::asio_engine_t::decode_and_push (msg_t *msg_)
{
    if (msg_->flags () & msg_t::command) {
        process_command_message (msg_);
    }

    if (_connection_facade.session->push_msg (msg_) == -1) {
        if (asio_trace_on) {
            fprintf (stderr, "[ASIO_TRACE] push_msg failed size=%zu flags=0x%x errno=%d\n",
                     msg_->size (), msg_->flags (), errno);
        }
        if (errno == EAGAIN)
            _process_msg = &asio_engine_t::push_one_then_decode_and_push;
        return -1;
    }
    if (asio_trace_on) {
        fprintf (stderr, "[ASIO_TRACE] push_msg ok size=%zu flags=0x%x\n", msg_->size (),
                 msg_->flags ());
    }
    return 0;
}

int zlink::asio_engine_t::push_one_then_decode_and_push (msg_t *msg_)
{
    const int rc = _connection_facade.session->push_msg (msg_);
    if (rc == 0)
        _process_msg = &asio_engine_t::decode_and_push;
    return rc;
}

int zlink::asio_engine_t::pull_msg_from_session (msg_t *msg_)
{
    return _connection_facade.session->pull_msg (msg_);
}

int zlink::asio_engine_t::push_msg_to_session (msg_t *msg_)
{
    return _connection_facade.session->push_msg (msg_);
}

void zlink::asio_engine_t::error (error_reason_t reason_)
{
    ENGINE_DBG ("error: reason=%d", static_cast<int> (reason_));

    //  Mark as terminating to prevent callbacks from processing
    if (_connection_facade.terminating)
        return;
    _connection_facade.terminating = true;
    _connection_facade.callback_guard.reset ();

    zlink_assert (_connection_facade.session);

    // protocol errors have been signaled already at the point where they occurred
    if (reason_ != protocol_error && _connection_facade.handshaking) {
        const int err = errno;
        _connection_facade.socket->event_handshake_failed_no_detail (_endpoint_uri_pair, err);
    }

    uint64_t disconnect_reason = ZLINK_DISCONNECT_UNKNOWN;
    if (_connection_facade.socket && _connection_facade.socket->is_ctx_terminated ()) {
        disconnect_reason = ZLINK_DISCONNECT_CTX_TERM;
    } else if (_connection_facade.handshaking || reason_ == protocol_error) {
        disconnect_reason = ZLINK_DISCONNECT_HANDSHAKE_FAILED;
    } else if (reason_ == timeout_error || reason_ == connection_error) {
        disconnect_reason = ZLINK_DISCONNECT_TRANSPORT_ERROR;
    }

    const zlink::blob_t *routing_id =
      _connection_facade.session ? &_connection_facade.session->peer_routing_id () : NULL;
    const unsigned char *routing_id_data = routing_id ? routing_id->data () : NULL;
    const size_t routing_id_size = routing_id ? routing_id->size () : 0;
    _connection_facade.socket->event_disconnected (_endpoint_uri_pair, disconnect_reason,
                                                   routing_id_data, routing_id_size);
    _connection_facade.session->flush ();
    _connection_facade.session->engine_error (!_connection_facade.handshaking, reason_);
    unplug ();

    destroy_after_callbacks ();
}

void zlink::asio_engine_t::destroy_after_callbacks ()
{
    if (_transport_adapter.io_context) {
        boost::asio::post (*_transport_adapter.io_context,
                           [this] () { zlink::release_heap_owned (this); });
        return;
    }

    zlink::release_heap_owned (this);
}

void zlink::asio_engine_t::set_handshake_timer ()
{
    zlink_assert (!_has_handshake_timer);

    if (_options.handshake_ivl > 0) {
        add_timer (_options.handshake_ivl, handshake_timer_id);
        _has_handshake_timer = true;
    }
}

void zlink::asio_engine_t::cancel_handshake_timer ()
{
    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }
}

void zlink::asio_engine_t::add_timer (int timeout_, int id_)
{
    ENGINE_DBG ("add_timer: timeout=%d, id=%d", timeout_, id_);

    zlink_assert (_transport_adapter.timer);
    _transport_adapter.current_timer_id = id_;
    _transport_adapter.timer->expires_after (std::chrono::milliseconds (timeout_));
    _transport_adapter.timer->async_wait (
      [this, id_,
       callback_guard = std::weak_ptr<connection_facade_t::callback_guard_t> (
         _connection_facade.callback_guard)] (const boost::system::error_code &ec) {
          if (callback_guard.expired ())
              return;
          on_timer (id_, ec);
      });
}

void zlink::asio_engine_t::cancel_timer (int id_)
{
    ENGINE_DBG ("cancel_timer: id=%d", id_);

    if (_transport_adapter.current_timer_id == id_ && _transport_adapter.timer) {
        _transport_adapter.timer->cancel ();
        _transport_adapter.current_timer_id = -1;
    }
}

void zlink::asio_engine_t::on_timer (int id_, const boost::system::error_code &ec)
{
    ENGINE_DBG ("on_timer: id=%d, ec=%s, terminating=%d", id_, ec.message ().c_str (),
                _connection_facade.terminating);

    if (ec == boost::asio::error::operation_aborted)
        return;

    //  If terminating, just return - terminate() is draining handlers
    if (_connection_facade.terminating)
        return;

    if (!_connection_facade.plugged)
        return;

    if (id_ == handshake_timer_id) {
        _has_handshake_timer = false;
        //  handshake timer expired before handshake completed, so engine fail
        error (timeout_error);
    }
}

bool zlink::asio_engine_t::init_properties (properties_t &properties_)
{
    if (_peer_address.empty ())
        return false;
    properties_.ZLINK_MAP_INSERT_OR_EMPLACE (std::string (ZLINK_MSG_PROPERTY_PEER_ADDRESS),
                                             _peer_address);
    return true;
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO
