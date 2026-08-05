/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include "engine/asio/asio_zmp_engine.hpp"
#include "protocol/zmp_protocol.hpp"
#include "protocol/zmp_control.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_encoder.hpp"
#include "protocol/zmp_decoder.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"
#include "protocol/wire.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"

#if defined ZLINK_HAVE_ASIO_SSL
#include <boost/asio/ssl.hpp>
#endif

#include <algorithm>
#include <limits.h>
#include <string.h>

zlink::asio_zmp_engine_t::asio_zmp_engine_t (fd_t fd_,
                                             const options_t &options_,
                                             const endpoint_uri_pair_t &endpoint_uri_pair_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_),
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
    _last_error_code (0)
{
    init_zmp_engine ();
}

zlink::asio_zmp_engine_t::asio_zmp_engine_t (fd_t fd_,
                                             const options_t &options_,
                                             const endpoint_uri_pair_t &endpoint_uri_pair_,
                                             std::unique_ptr<i_asio_transport> transport_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_, std::move (transport_)),
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
    _last_error_code (0)
{
    init_zmp_engine ();
}

#if defined ZLINK_HAVE_ASIO_SSL
zlink::asio_zmp_engine_t::asio_zmp_engine_t (
  fd_t fd_,
  const options_t &options_,
  const endpoint_uri_pair_t &endpoint_uri_pair_,
  std::unique_ptr<i_asio_transport> transport_,
  std::unique_ptr<boost::asio::ssl::context> ssl_context_) :
    asio_engine_t (fd_, options_, endpoint_uri_pair_, std::move (transport_)),
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
    _last_error_code (0),
    _ssl_context (std::move (ssl_context_))
{
    init_zmp_engine ();
}
#endif

zlink::asio_zmp_engine_t::~asio_zmp_engine_t ()
{
}

void zlink::asio_zmp_engine_t::init_zmp_engine ()
{
    _next_msg =
      static_cast<int (asio_engine_t::*) (msg_t *)> (&asio_zmp_engine_t::pull_msg_from_session);
    _process_msg =
      static_cast<int (asio_engine_t::*) (msg_t *)> (&asio_zmp_engine_t::decode_and_push);

    memset (_hello_recv, 0, sizeof (_hello_recv));
    memset (_hello_send, 0, sizeof (_hello_send));
    memset (_peer_routing_id, 0, sizeof (_peer_routing_id));
    _peer_routing_id_size = 0;
    _ready_send.clear ();
    _deferred_ready_send.clear ();
    _deferred_ready_pending = false;
    _ready_sent = false;
    _ready_received = false;
    _last_error_code = 0;
    _last_error_reason.clear ();
}

void zlink::asio_zmp_engine_t::set_last_error (uint8_t code_, const char *reason_)
{
    _last_error_code = code_;
    if (reason_ && *reason_)
        _last_error_reason.assign (reason_);
    else
        _last_error_reason.assign (zmp_error_reason (code_));
}

void zlink::asio_zmp_engine_t::send_error_frame (uint8_t code_, const char *reason_)
{
    i_asio_transport *tr = transport ();
    if (!tr || !tr->is_open ())
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
        const std::size_t written = tr->write_some (buffer + offset, total - offset);
        if (written == 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }
        offset += written;
    }
}

void zlink::asio_zmp_engine_t::error (error_reason_t reason_)
{
    if (reason_ == timeout_error) {
        if (is_handshaking ())
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

    asio_engine_t::error (reason_);
}

void zlink::asio_zmp_engine_t::plug_internal ()
{
    set_handshake_timer ();

    const bool defer_ready =
      paired_transport () && !_options.transport_pair_initiator;
    if (defer_ready) {
        zmp_control::build_hello_frame (
          _options, _hello_send, sizeof (_hello_send), &_hello_send_size);
        _ready_send.assign (
          _hello_send, _hello_send + _hello_send_size);
        _ready_sent = false;
    } else {
        zmp_control::build_hello_ready_frames (
          _options, _hello_send, sizeof (_hello_send), &_hello_send_size,
          _ready_send);
        _ready_sent = true;
    }
    _outpos = &_ready_send[0];
    _outsize = _ready_send.size ();
    _hello_sent = true;

    if (_options.type == ZLINK_CORE_SOCKET_PUB || _options.type == ZLINK_CORE_SOCKET_XPUB)
        _subscription_required = true;

    start_async_read ();
    start_async_write ();
}

bool zlink::asio_zmp_engine_t::handshake ()
{
    if (!_hello_received) {
        if (!receive_hello ()) {
            errno = EAGAIN;
            return false;
        }
    }

    if (!_hello_sent) {
        errno = EAGAIN;
        return false;
    }

    if (_encoder == NULL) {
        _encoder = new (std::nothrow) zmp_encoder_t (_options.out_batch_size);
        alloc_assert (_encoder);
    }

    if (_decoder == NULL) {
        _decoder = new (std::nothrow) zmp_decoder_t (_options.in_batch_size, _options.maxmsgsize);
        alloc_assert (_decoder);
        _input_in_decoder_buffer = false;
    }

    if (!process_handshake_input ())
        return false;

    if (!_ready_received) {
        errno = EAGAIN;
        return false;
    }

    if (_has_handshake_stage) {
        session ()->set_peer_routing_id (_peer_routing_id, _peer_routing_id_size);
        session ()->engine_ready ();
        _has_handshake_stage = false;
    } else {
        session ()->set_peer_routing_id (_peer_routing_id, _peer_routing_id_size);
    }

    if (_options.recv_routing_id) {
        msg_t routing_id;
        const int rc = routing_id.init_size (_peer_routing_id_size);
        errno_assert (rc == 0);
        if (_peer_routing_id_size > 0)
            memcpy (routing_id.data (), _peer_routing_id, _peer_routing_id_size);
        routing_id.set_flags (msg_t::routing_id);
        const int push_rc = session ()->push_msg (&routing_id);
        if (push_rc == -1 && errno == EAGAIN) {
            // Align with libzmq behavior: during shutdown races the session
            // pipe can disappear before routing-id is forwarded.
            return false;
        }
        errno_assert (push_rc == 0);
        session ()->flush ();
    }

    if (_has_handshake_timer) {
        cancel_timer (handshake_timer_id);
        _has_handshake_timer = false;
    }

    if (!paired_transport ()) {
        socket ()->event_connection_ready_changed (
          _endpoint_uri_pair, _peer_routing_id, _peer_routing_id_size);
    } else {
        socket ()->event_transport_pair_lane_ready (
          _endpoint_uri_pair, _peer_routing_id, _peer_routing_id_size,
          _negotiated_transport_lane, _negotiated_transport_pair_id,
          _negotiated_transport_pair_generation);
    }

    if (_output_stopped)
        restart_output ();
    else
        start_async_write ();

    _last_error_code = 0;
    _last_error_reason.clear ();
    return true;
}

bool zlink::asio_zmp_engine_t::receive_hello ()
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

bool zlink::asio_zmp_engine_t::parse_hello (const unsigned char *data_, size_t size_)
{
    zmp_control::hello_parse_result_t result;
    if (zmp_control::parse_hello_frame (data_, size_, _options.type, &result) != 0) {
        set_last_error (result.error_code, result.error_reason);
        if (result.malformed_hello_event) {
            socket ()->event_handshake_failed_protocol (
              session ()->get_endpoint (), ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_HELLO);
        }
        error (protocol_error);
        return false;
    }

    _peer_routing_id_size = result.identity_len;
    if (result.identity_len > 0)
        memcpy (_peer_routing_id, result.identity, result.identity_len);

    return true;
}

bool zlink::asio_zmp_engine_t::process_handshake_input ()
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

int zlink::asio_zmp_engine_t::process_ready_message (msg_t *msg_)
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
    session ()->set_peer_max_message_bytes (peer_max_message_bytes);

    transport_lane_t lane = transport_lane_application;
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    const int pair_rc =
      zmp_metadata::parse_transport_pair (properties, &lane, &pair_id, &generation);
    if (pair_rc < 0 || (paired_transport () && pair_rc == 0)
        || (!paired_transport () && pair_rc != 0)
        || (pair_rc > 0
            && session ()->set_peer_transport_pair (lane, pair_id, generation) != 0)) {
        set_last_error (zmp_error_internal, "transport pair metadata invalid");
        return -1;
    }
    if (pair_rc > 0) {
        _negotiated_transport_lane = lane;
        _negotiated_transport_pair_id = pair_id;
        _negotiated_transport_pair_generation = generation;
    }
    if (paired_transport () && !_options.transport_pair_initiator)
        schedule_ready_reply (lane, pair_id, generation);

    return 0;
}

bool zlink::asio_zmp_engine_t::paired_transport () const
{
    return _options.type == ZLINK_CORE_SOCKET_DEALER
           || _options.type == ZLINK_CORE_SOCKET_ROUTER;
}

void zlink::asio_zmp_engine_t::schedule_ready_reply (
  transport_lane_t lane_, uint64_t pair_id_, uint64_t generation_)
{
    options_t response_options = _options;
    response_options.zmp_metadata = true;
    response_options.transport_lane = lane_;
    response_options.transport_pair_id = pair_id_;
    response_options.transport_pair_generation = generation_;
    zmp_control::build_ready_frame (response_options, _deferred_ready_send);
    _deferred_ready_pending = true;
    if (_outsize == 0 && prepare_deferred_handshake_output ())
        start_async_write ();
}

bool zlink::asio_zmp_engine_t::prepare_deferred_handshake_output ()
{
    if (!_deferred_ready_pending || _outsize != 0)
        return false;
    _outpos = &_deferred_ready_send[0];
    _outsize = _deferred_ready_send.size ();
    _deferred_ready_pending = false;
    _ready_sent = true;
    return true;
}

int zlink::asio_zmp_engine_t::process_error_message (msg_t *msg_)
{
    uint8_t code = zmp_error_internal;
    const char *error_reason = NULL;
    zmp_control::parse_error_frame (msg_, &code, &error_reason);
    set_last_error (code, error_reason);
    errno = EPROTO;
    return -1;
}

int zlink::asio_zmp_engine_t::decode_and_push (msg_t *msg_)
{
    const unsigned char msg_flags = msg_->flags ();
    if (msg_flags & msg_t::command) {
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

    if ((msg_flags & msg_t::routing_id) && !_options.recv_routing_id) {
        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    if (session ()->push_msg (msg_) == -1) {
        if (errno == EAGAIN)
            _process_msg = static_cast<int (asio_engine_t::*) (msg_t *)> (
              &asio_zmp_engine_t::push_one_then_decode);
        return -1;
    }
    return 0;
}

int zlink::asio_zmp_engine_t::process_command_message (msg_t *msg_)
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

bool zlink::asio_zmp_engine_t::build_gather_header (const msg_t &msg_,
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

int zlink::asio_zmp_engine_t::push_one_then_decode (msg_t *msg_)
{
    const int rc = session ()->push_msg (msg_);
    if (rc == 0)
        _process_msg =
          static_cast<int (asio_engine_t::*) (msg_t *)> (&asio_zmp_engine_t::decode_and_push);
    return rc;
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO
