/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_ENGINE_HPP_INCLUDED__
#define __ZLINK_ASIO_ENGINE_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <boost/asio.hpp>

#include <memory>
#include <vector>
#include <deque>

#include "utils/fd.hpp"
#include "engine/i_engine.hpp"
#include "core/options.hpp"
#include "core/endpoint.hpp"
#include "protocol/i_encoder.hpp"
#include "protocol/i_decoder.hpp"
#include "core/msg.hpp"
#include "protocol/metadata.hpp"
#include "engine/asio/handler_allocator.hpp"
#include "engine/asio/asio_engine_pipeline.hpp"
#include "engine/asio/i_asio_transport.hpp"

namespace zlink
{
class io_thread_t;
class session_base_t;
class socket_base_t;
class i_asio_transport;

//  True Proactor Mode ASIO Engine
//
//  This engine uses Boost.Asio's async_read/async_write for true
//  asynchronous I/O operations, as opposed to the reactor mode
//  (async_wait for readiness) used by asio_poller.
//
//  The engine manages read/write buffers internally and handles
//  completion callbacks to drive the ZMP protocol.

class asio_engine_t : public i_engine
{
  public:
    asio_engine_t (
      fd_t fd_,
      const options_t &options_,
      const endpoint_uri_pair_t &endpoint_uri_pair_,
      std::unique_ptr<i_asio_transport> transport_ = std::unique_ptr<i_asio_transport> ());
    ~asio_engine_t () ZLINK_OVERRIDE;

    //  i_engine interface implementation.
    bool has_handshake_stage () ZLINK_OVERRIDE { return _has_handshake_stage; }
    void plug (zlink::io_thread_t *io_thread_, zlink::session_base_t *session_) ZLINK_OVERRIDE;
    void terminate () ZLINK_OVERRIDE;
    bool restart_input () ZLINK_OVERRIDE;
    void restart_output () ZLINK_OVERRIDE;
    const endpoint_uri_pair_t &get_endpoint () const ZLINK_OVERRIDE;

  protected:
    typedef metadata_t::dict_t properties_t;
    bool init_properties (properties_t &properties_);

    //  Function to handle network disconnections.
    virtual void error (error_reason_t reason_);

    int pull_msg_from_session (msg_t *msg_);
    int push_msg_to_session (msg_t *msg_);

    virtual int decode_and_push (msg_t *msg_);
    int push_one_then_decode_and_push (msg_t *msg_);

    virtual bool handshake () { return true; }
    virtual void plug_internal () {}
    virtual bool prepare_deferred_handshake_output () { return false; }

    virtual int process_command_message (msg_t *msg_)
    {
        LIBZLINK_UNUSED (msg_);
        return -1;
    }
    //  Build protocol-specific header for gather write.
    //  Returns true on success and sets header_size_.
    virtual bool build_gather_header (const msg_t &msg_,
                                      unsigned char *buffer_,
                                      size_t buffer_size_,
                                      size_t &header_size_);

    //  Start asynchronous read operation
    void start_async_read ();

    //  Start asynchronous write operation
    void start_async_write ();

    //  Speculative (synchronous) write attempt.
    //  Tries to write immediately using transport->write_some().
    //  Falls back to async write if would_block or partial write occurs.
    void speculative_write ();

    //  Handle read completion
    void on_read_complete (const boost::system::error_code &ec, std::size_t bytes_transferred);

    //  Handle write completion
    void on_write_complete (const boost::system::error_code &ec, std::size_t bytes_transferred);

    //  Set up handshake timer
    void set_handshake_timer ();

    //  Cancel handshake timer
    void cancel_handshake_timer ();

    //  Timer callback
    void on_timer (int id_, const boost::system::error_code &ec);

    //  Access to session and socket
    session_base_t *session () { return _connection_facade.session; }
    socket_base_t *socket () { return _connection_facade.socket; }
    i_asio_transport *transport () { return _transport_adapter.transport.get (); }
    bool is_handshaking () const { return _connection_facade.handshaking; }

    const options_t _options;

    //  Buffers for async I/O
    unsigned char *_inpos;
    size_t _insize;
    i_decoder *_decoder;
    //  True when _inpos/_insize refer to data read into the decoder buffer.
    bool _input_in_decoder_buffer;

    unsigned char *_outpos;
    size_t _outsize;
    i_encoder *_encoder;

    int (asio_engine_t::*_next_msg) (msg_t *msg_);
    int (asio_engine_t::*_process_msg) (msg_t *msg_);

    //  Metadata to be attached to received messages. May be NULL.
    metadata_t *_metadata;

    //  True iff the engine couldn't consume the last decoded message.
    bool _input_stopped;

    //  True iff the engine doesn't have any message to encode.
    bool _output_stopped;

    //  Representation of the connected endpoints.
    const endpoint_uri_pair_t _endpoint_uri_pair;

    //  ID of the handshake timer
    enum
    {
        handshake_timer_id = 0x40
    };

    //  True if handshake timer is running.
    bool _has_handshake_timer;

    const std::string _peer_address;

    //  Indicate if engine has a handshake stage
    bool _has_handshake_stage;

    //  Add a timer using ASIO
    void add_timer (int timeout_, int id_);

    //  Cancel a timer
    void cancel_timer (int id_);

    //  Start transport handshake if required
    void start_transport_handshake ();

    //  Transport handshake completion handler
    void on_transport_handshake (const boost::system::error_code &ec);

    //  Complete protocol handshake immediately for raw/handshake-less engines.
    void complete_handshake ();

  private:
    //  Process incoming data after async read completes
    bool process_input ();

    //  Fill output buffer and start async write
    void process_output ();

    //  Internal implementation of restart_input
    bool restart_input_internal ();

    //  Attempt a synchronous read to drain immediately available data.
    //  Returns true if a read was attempted or an error occurred.
    bool speculative_read ();

    //  Drain a bounded number of immediately available STREAM reads after an
    //  async callback to reduce callback churn on small payloads.
    void maybe_drain_stream_reads ();

    bool buffer_stream_backpressure_read (size_t bytes_transferred_);

    //  Prepare output buffer from encoder (called by speculative_write).
    //  Returns true if data is available in _outpos/_outsize.
    bool prepare_output_buffer ();

    //  Prepare gather write (header + body) for large messages.
    //  Returns true if gather write was scheduled or output was stopped.
    bool prepare_gather_output ();

    //  Finalize message state after gather write completion.
    void finish_gather_output ();

    //  Unplug the engine from the session.
    void unplug ();

    void destroy_after_callbacks ();

    bool is_tcp_transport () const;
    bool use_stream_speculative_write () const;
    bool use_non_tcp_speculative_read () const;
    bool use_stream_rx_slab () const;
    bool use_stream_dynamic_read_growth () const;
    bool use_stream_dynamic_write_growth () const;
    void prime_stream_decoder_read_target ();
    void maybe_grow_stream_decoder_read_target (size_t bytes_transferred_);
    void apply_pending_stream_encoder_resize ();
    void maybe_schedule_stream_encoder_growth (size_t filled_out_batch_);

    //  Point read_buffer_ptr at the handshake buffer (allocated on first
    //  use) and return its size.
    size_t select_handshake_read_buffer ();

    static const size_t read_buffer_size = 8192;

    //  Handshake-phase reads (no decoder yet) use a small lazily-allocated
    //  buffer; frames are parsed incrementally so a small buffer only costs
    //  extra reads. Sizing background: core/study/connection-memory-study.ko.md §6.1.
    static const size_t handshake_read_buffer_size = 512;
    enum
    {
        pending_buffer_pool_max = 4
    };

    //  Maximum total size of pending buffers (10MB default)
    static const size_t max_pending_buffer_size = 10 * 1024 * 1024;

    //  STREAM-only custom allocator slots for high-frequency callbacks.
    handler_allocator _stream_read_handler_allocator;
    handler_allocator _stream_write_handler_allocator;
    handler_allocator _stream_handshake_handler_allocator;

    struct transport_adapter_t
    {
        transport_adapter_t () : io_context (NULL), current_timer_id (-1), fd (retired_fd) {}

        boost::asio::io_context *io_context;
        std::unique_ptr<i_asio_transport> transport;
        std::unique_ptr<boost::asio::steady_timer> timer;
        int current_timer_id;
        fd_t fd;
    };

    struct connection_facade_t
    {
        struct callback_guard_t
        {
        };

        connection_facade_t () :
            plugged (false), handshaking (true), terminating (false), session (NULL), socket (NULL)
        {
        }

        bool plugged;
        bool handshaking;
        bool terminating;
        std::shared_ptr<callback_guard_t> callback_guard;
        zlink::session_base_t *session;
        zlink::socket_base_t *socket;
    };

    transport_adapter_t _transport_adapter;
    asio_engine_pipeline_t _pipeline;
    connection_facade_t _connection_facade;

  public:
    size_t stream_encoder_write_target_size () const
    {
        return _pipeline.stream_encoder_write_target_size;
    }

    ZLINK_NON_COPYABLE_NOR_MOVABLE (asio_engine_t)
};
} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

#endif // __ZLINK_ASIO_ENGINE_HPP_INCLUDED__
