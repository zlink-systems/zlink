/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include "engine/asio/asio_raw_engine.hpp"
#include "protocol/raw_encoder.hpp"
#include "protocol/raw_decoder.hpp"
#include "utils/err.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/session_base.hpp"

zlink::asio_raw_engine_t::asio_raw_engine_t (fd_t fd_,
                                             const options_t &options_,
                                             const endpoint_uri_pair_t &endpoint_uri_pair_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_)
{
    init_raw_engine ();
}

zlink::asio_raw_engine_t::asio_raw_engine_t (fd_t fd_,
                                             const options_t &options_,
                                             const endpoint_uri_pair_t &endpoint_uri_pair_,
                                             std::unique_ptr<i_asio_transport> transport_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_, std::move (transport_))
{
    init_raw_engine ();
}

#if defined ZLINK_HAVE_ASIO_SSL
zlink::asio_raw_engine_t::asio_raw_engine_t (
  fd_t fd_,
  const options_t &options_,
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  std::unique_ptr<i_asio_transport> transport_,
  std::unique_ptr<boost::asio::ssl::context> ssl_context_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_, std::move (transport_)),
    _ssl_context (std::move (ssl_context_))
{
    init_raw_engine ();
}
#endif

zlink::asio_raw_engine_t::~asio_raw_engine_t ()
{
}

void zlink::asio_raw_engine_t::init_raw_engine ()
{
    _next_msg =
      static_cast<int (asio_engine_t::*) (msg_t *)> (&asio_raw_engine_t::pull_msg_from_session);
    _process_msg =
      static_cast<int (asio_engine_t::*) (msg_t *)> (&asio_raw_engine_t::decode_and_push);
}

void zlink::asio_raw_engine_t::plug_internal ()
{
    if (_encoder == NULL) {
        _encoder = new (std::nothrow) raw_encoder_t (_options.out_batch_size);
        alloc_assert (_encoder);
    }

    if (_decoder == NULL) {
        size_t initial_read_buffer = _options.in_batch_size > 0
                                       ? static_cast<size_t> (_options.in_batch_size)
                                       : static_cast<size_t> (4096);
        if (initial_read_buffer > static_cast<size_t> (8192))
            initial_read_buffer = static_cast<size_t> (8192);
        if (_options.rcvbuf > 0 && static_cast<size_t> (_options.rcvbuf) < initial_read_buffer) {
            initial_read_buffer = static_cast<size_t> (_options.rcvbuf);
        }
        if (_options.maxmsgsize > 0
            && static_cast<size_t> (_options.maxmsgsize) < initial_read_buffer) {
            initial_read_buffer = static_cast<size_t> (_options.maxmsgsize);
        }
        if (initial_read_buffer == 0)
            initial_read_buffer = 1;

        size_t stream_max_read_buffer = initial_read_buffer;
        if (_options.rcvbuf > 0 && static_cast<size_t> (_options.rcvbuf) > stream_max_read_buffer)
            stream_max_read_buffer = static_cast<size_t> (_options.rcvbuf);
        if (_options.maxmsgsize > 0
            && static_cast<size_t> (_options.maxmsgsize) < stream_max_read_buffer)
            stream_max_read_buffer = static_cast<size_t> (_options.maxmsgsize);
        if (stream_max_read_buffer == 0)
            stream_max_read_buffer = initial_read_buffer;

        _decoder = new (std::nothrow)
          raw_decoder_t (initial_read_buffer, _options.maxmsgsize, stream_max_read_buffer);
        alloc_assert (_decoder);
        _input_in_decoder_buffer = false;
    }

    properties_t properties;
    if (init_properties (properties)) {
        zlink_assert (_metadata == NULL);
        _metadata = new (std::nothrow) metadata_t (properties);
        alloc_assert (_metadata);
    }

    complete_handshake ();
    //  STREAM assigns its four-byte session routing id when the socket-side
    //  pipe is attached. Clearing the peer id from the I/O thread races that
    //  assignment on the peer pipe and has no protocol meaning for STREAM.
    if (session () && _options.type != ZLINK_CORE_SOCKET_STREAM)
        session ()->set_peer_routing_id (NULL, 0);
    if (_options.type != ZLINK_CORE_SOCKET_STREAM)
        socket ()->event_connection_ready_changed (_endpoint_uri_pair, NULL, 0);

    start_async_read ();
    start_async_write ();
}

bool zlink::asio_raw_engine_t::build_gather_header (const msg_t &msg_,
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

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO
