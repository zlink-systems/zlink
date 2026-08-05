/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_WS_ENGINE_HPP_INCLUDED__
#define __ZLINK_ASIO_WS_ENGINE_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_WS

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>

#include "utils/fd.hpp"
#include "engine/i_engine.hpp"
#include "core/options.hpp"
#include "core/endpoint.hpp"
#include "protocol/i_encoder.hpp"
#include "protocol/i_decoder.hpp"
#include "core/msg.hpp"
#include "protocol/metadata.hpp"
#include "protocol/zmp_metadata.hpp"
#include "engine/asio/i_asio_transport.hpp"

#if defined ZLINK_HAVE_ASIO_SSL
namespace boost
{
namespace asio
{
namespace ssl
{
class context;
}
}
}
#endif

namespace zlink
{

class io_thread_t;
class session_base_t;
class socket_base_t;

//  WebSocket ZMP Engine
//
//  This engine implements ZMP protocol over WebSocket transport.
//  It uses ws_transport_t for WebSocket framing and Beast I/O,
//  while implementing the ZMP handshake and message protocol.

class asio_ws_engine_t ZLINK_FINAL : public i_engine
{
  public:
    struct callback_guard_t
    {
    };

    //  Create WebSocket engine for an already-connected socket
    //  fd_: The socket file descriptor (TCP connection already established)
    //  options_: ZLINK socket options
    //  endpoint_uri_pair_: Local and remote endpoint URIs
    //  host_: WebSocket host for client handshake
    //  path_: WebSocket path (e.g., "/zlink")
    //  is_client_: true for client-side (zlink_connect), false for server-side (zlink_bind)
    asio_ws_engine_t (fd_t fd_,
                      const options_t &options_,
                      const endpoint_uri_pair_t &endpoint_uri_pair_,
                      bool is_client_,
                      std::unique_ptr<i_asio_transport> transport_);
#if defined ZLINK_HAVE_ASIO_SSL
    asio_ws_engine_t (fd_t fd_,
                      const options_t &options_,
                      const endpoint_uri_pair_t &endpoint_uri_pair_,
                      bool is_client_,
                      std::unique_ptr<i_asio_transport> transport_,
                      std::unique_ptr<boost::asio::ssl::context> ssl_context_);
#endif

    ~asio_ws_engine_t () ZLINK_OVERRIDE;

    //  i_engine interface implementation
    bool has_handshake_stage () ZLINK_OVERRIDE { return true; }
    void plug (zlink::io_thread_t *io_thread_, zlink::session_base_t *session_) ZLINK_OVERRIDE;
    void terminate () ZLINK_OVERRIDE;
    bool restart_input () ZLINK_OVERRIDE;
    void restart_output () ZLINK_OVERRIDE;
    const endpoint_uri_pair_t &get_endpoint () const ZLINK_OVERRIDE;

  protected:
    typedef metadata_t::dict_t properties_t;
    bool init_properties (properties_t &properties_);

    void error (error_reason_t reason_);

    int pull_msg_from_session (msg_t *msg_);
    int push_msg_to_session (msg_t *msg_);

    int pull_and_encode (msg_t *msg_);
    int decode_and_push (msg_t *msg_);
    int push_one_then_decode_and_push (msg_t *msg_);

    int process_command_message (msg_t *msg_);
    void set_last_error (uint8_t code_, const char *reason_);
    void send_error_frame (uint8_t code_, const char *reason_);

  private:
    //  Start WebSocket handshake
    void start_ws_handshake ();

    //  WebSocket handshake completion callback
    void on_ws_handshake_complete (const boost::system::error_code &ec);

    //  Start ZMP handshake after WebSocket handshake completes
    void start_zmp_handshake ();

    //  Gather write helpers (ZMP header + body)
    bool build_gather_header (const msg_t &msg_,
                              unsigned char *buffer_,
                              size_t buffer_size_,
                              size_t &header_size_);
    bool prepare_gather_output ();
    void finish_gather_output ();

    //  Handshake methods
    bool handshake ();
    bool handshake_zmp ();

    bool receive_hello ();
    bool parse_hello (const unsigned char *data_, size_t size_);
    bool process_zmp_handshake_input ();
    int process_ready_message (msg_t *msg_);
    int process_error_message (msg_t *msg_);
    bool paired_transport () const;
    void schedule_ready_reply (transport_lane_t lane_,
                               uint64_t pair_id_,
                               uint64_t generation_);
    bool prepare_deferred_ready_reply ();

    //  Async I/O methods
    void start_async_read ();
    void start_async_write ();

    //  Speculative (synchronous) write attempt.
    //  Tries to write immediately using transport->write_some().
    //  Falls back to async write if would_block or partial write occurs.
    void speculative_write ();

    bool use_stream_dynamic_read_growth () const;
    bool use_stream_dynamic_write_growth () const;
    void prime_stream_decoder_read_target ();
    void maybe_grow_stream_decoder_read_target (size_t bytes_transferred_);
    void apply_pending_stream_encoder_resize ();
    void maybe_schedule_stream_encoder_growth (size_t filled_out_batch_);

    //  Prepare output buffer from encoder.
    //  Returns true if data is available in _outpos/_outsize.
    bool prepare_output_buffer ();

    void on_read_complete (const boost::system::error_code &ec, std::size_t bytes_transferred);
    void on_write_complete (const boost::system::error_code &ec, std::size_t bytes_transferred);

    //  Internal data processing
    bool process_input ();
    void process_output ();

    //  Timer management
    void add_timer (int timeout_, int id_);
    void cancel_timer (int id_);
    void set_handshake_timer ();
    void cancel_handshake_timer ();
    void on_timer (int id_, const boost::system::error_code &ec);
    void schedule_terminate_completion ();
    void destroy_after_callbacks ();

    //  WebSocket transport layer
    std::unique_ptr<i_asio_transport> _transport;

    //  WebSocket-specific data
    bool _is_client;
    bool _ws_handshake_complete;

    //  Session and socket
    session_base_t *_session;
    socket_base_t *_socket;

    //  File descriptor for initial open
    fd_t _fd;

    //  IO context pointer (set during plug)
    boost::asio::io_context *_io_context;
    std::shared_ptr<callback_guard_t> _callback_guard;

    //  Options
    const options_t _options;

    //  Endpoint pair
    const endpoint_uri_pair_t _endpoint_uri_pair;

    //  Peer address string
    const std::string _peer_address;

    //  Buffers for ZMP I/O
    unsigned char *_inpos;
    size_t _insize;
    i_decoder *_decoder;
    bool _input_in_decoder_buffer;

    unsigned char *_outpos;
    size_t _outsize;
    i_encoder *_encoder;

    //  Message function pointers
    int (asio_ws_engine_t::*_next_msg) (msg_t *msg_);
    int (asio_ws_engine_t::*_process_msg) (msg_t *msg_);

    //  Metadata attached to received messages
    metadata_t *_metadata;

    //  State flags
    bool _plugged;
    bool _handshaking;
    bool _input_stopped;
    bool _output_stopped;
    bool _io_error;
    bool _read_pending;
    bool _write_pending;
    bool _terminating;
    uint8_t _last_error_code;
    std::string _last_error_reason;

    //  Internal read buffer
    static const size_t read_buffer_size = 8192;
    std::vector<unsigned char> _read_buffer;
    unsigned char *_read_buffer_ptr;
    size_t _last_read_request_size;
    bool _last_read_had_partial_prefix;
    size_t _stream_decoder_read_target_size;
    size_t _stream_decoder_read_target_max;
    size_t _stream_decoder_read_target_full_hits;
    size_t _stream_encoder_write_target_size;
    size_t _stream_encoder_write_target_max;
    size_t _stream_encoder_write_target_full_hits;
    size_t _stream_encoder_pending_resize_size;

    //  Outgoing message
    msg_t _tx_msg;

    //  Gather write state (header + body)
    bool _async_gather;
    unsigned char _gather_header[64];
    size_t _gather_header_size;
    const unsigned char *_gather_body;
    size_t _gather_body_size;

    //  ZMP handshake
    static const size_t zmp_hello_buf_size = 272;

    bool _hello_sent;
    bool _hello_received;
    bool _ready_sent;
    bool _ready_received;
    size_t _hello_header_bytes;
    size_t _hello_body_bytes;
    uint32_t _hello_body_len;
    unsigned char _hello_recv[zmp_hello_buf_size];
    unsigned char _hello_send[zmp_hello_buf_size];
    size_t _hello_send_size;
    std::vector<unsigned char> _ready_send;
    std::vector<unsigned char> _deferred_ready_send;
    bool _deferred_ready_pending;
    transport_lane_t _negotiated_transport_lane;
    uint64_t _negotiated_transport_pair_id;
    uint64_t _negotiated_transport_pair_generation;
    unsigned char _peer_routing_id[256];
    size_t _peer_routing_id_size;

    bool _subscription_required;

    //  Routing ID message
    msg_t _routing_id_msg;

#if defined ZLINK_HAVE_ASIO_SSL
    std::unique_ptr<boost::asio::ssl::context> _ssl_context;
#endif

    //  Timer
    std::unique_ptr<boost::asio::steady_timer> _timer;
    int _current_timer_id;

    //  Timer IDs
    enum
    {
        handshake_timer_id = 0x40
    };

    bool _has_handshake_timer;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_ws_engine_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_WS

#endif // __ZLINK_ASIO_WS_ENGINE_HPP_INCLUDED__
