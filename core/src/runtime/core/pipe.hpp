/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PIPE_HPP_INCLUDED__
#define __ZLINK_PIPE_HPP_INCLUDED__

#include <atomic>
#include <memory>

#include "core/ypipe_base.hpp"
#include "utils/config.hpp"
#include "core/object.hpp"
#include "utils/stdint.hpp"
#include "utils/array.hpp"
#include "utils/blob.hpp"
#include "core/options.hpp"
#include "core/endpoint.hpp"
#include "core/msg.hpp"
#include "core/pipe_stream_packet_state.hpp"
#include "utils/fast_mutex.hpp"

namespace zlink
{
class pipe_t;

enum pipe_write_status_t
{
    pipe_write_ready = 0,
    pipe_write_hwm_full,
    pipe_write_inactive
};

enum pipe_message_admission_t : int
{
    pipe_message_admission_ready = 0,
    pipe_message_admission_hwm_full,
    pipe_message_admission_too_large,
    pipe_message_admission_inactive,
    pipe_message_admission_invalid
};

struct transport_lifetime_t
{
    explicit transport_lifetime_t (uint64_t connection_id_) :
        connection_id (connection_id_)
    {
    }
    std::atomic<uint64_t> connection_id;
};

//  Create a pipepair for bi-directional transfer of messages.
//  First HWM is for messages passed from first pipe to the second pipe.
//  Second HWM is for messages passed from second pipe to the first pipe.
//  Delay specifies how the pipe behaves when the peer terminates. If true
//  pipe receives all the pending messages before terminating, otherwise it
//  terminates straight away.
//  If conflate is true, only the most recently arrived message could be
//  read (older messages are discarded)
//  If session_pipe is true the ypipes use the smaller
//  session_pipe_granularity chunk (per-connection pipes); otherwise the
//  default message_pipe_granularity is used.
int pipepair (zlink::object_t *parents_[2],
              zlink::pipe_t *pipes_[2],
               const uint64_t hwms_[2],
              const bool conflate_[2],
              bool session_pipe_ = false);

struct i_pipe_events
{
    virtual ~i_pipe_events () ZLINK_DEFAULT;

    virtual void read_activated (zlink::pipe_t *pipe_) = 0;
    virtual void write_activated (zlink::pipe_t *pipe_) = 0;
    virtual void hiccuped (zlink::pipe_t *pipe_) = 0;
    virtual void pipe_peer_terminated (zlink::pipe_t *pipe_) = 0;
    virtual void pipe_terminated (zlink::pipe_t *pipe_) = 0;
};

//  Note that pipe can be stored in three different arrays.
//  The array of inbound pipes (1), the array of outbound pipes (2) and
//  the generic array of pipes to be deallocated (3).

class pipe_t ZLINK_FINAL : public object_t,
                           public array_item_t<1>,
                           public array_item_t<2>,
                           public array_item_t<3>
{
    template <typename T> friend void release_heap_owned (T *);
#ifdef ZLINK_BUILD_TESTS
    friend class session_termination_test_access_t;
#endif

    //  This allows pipepair to create pipe objects.
    friend int pipepair (zlink::object_t *parents_[2],
                         zlink::pipe_t *pipes_[2],
                         const uint64_t hwms_[2],
                         const bool conflate_[2],
                         bool session_pipe_);

  public:
    typedef pipe_stream_packet_state_t stream_packet_state_t;

    //  Specifies the object to send events to.
    void set_event_sink (i_pipe_events *sink_);

    //  Pipe endpoint can store an routing ID to be used by its clients.
    void set_server_socket_routing_id (uint32_t server_socket_routing_id_);
    uint32_t get_server_socket_routing_id () const;

    //  Pipe endpoint can store an opaque ID to be used by its clients.
    void set_router_socket_routing_id (const blob_t &router_socket_routing_id_);
    const blob_t &get_routing_id () const;
    pipe_t *get_peer () const;
    void set_peer_routing_id (const unsigned char *data_, size_t size_);
    void set_transport_peer_identity (const unsigned char *data_, size_t size_);
    const blob_t &get_transport_peer_identity () const;
    uint64_t get_msgs_written () const;
    uint64_t get_msgs_read () const;
    uint64_t get_bytes_written () const;
    uint64_t get_bytes_read () const;
    uint64_t get_snd_pending_msgs () const;
    uint64_t get_rcv_pending_msgs_approx () const;
    uint64_t get_snd_pending_bytes () const;
    uint64_t get_rcv_pending_bytes_approx () const;
    uint64_t get_oversize_message_admission_count () const;
    uint64_t get_oversize_message_admission_max_bytes () const;
    uint64_t get_connected_time () const;
    void refresh_write_credit (uint64_t peer_msgs_read_, uint64_t peer_bytes_read_);
    bool mark_stream_connect_event_emitted ();
    void reset_stream_connect_event_emitted ();
    bool mark_connection_ready_event_emitted ();
    void reset_connection_ready_event_emitted ();
    stream_packet_state_t &stream_packet_state ();
    const stream_packet_state_t &stream_packet_state () const;
    fast_mutex_t &stream_packet_dispatch_sync ();
    void reset_stream_packet_state ();

    //  Returns true if there is at least one message to read in the pipe.
    bool check_read ();

    //  Reads a message to the underlying pipe.
    bool read (msg_t *msg_);

    //  Checks whether messages can be written to the pipe. If the pipe is
    //  closed or if writing the message would cause high watermark the
    //  function returns false.
    bool check_write ();

    //  Checks whether messages can be written to the pipe and reports whether
    //  failure was caused by HWM or inactive pipe state.
    pipe_write_status_t check_write_status ();

    // Paired transports expose the Application route before both physical
    // lanes finish their handshake so a blocking send can wait on EAGAIN.
    // The existing outbound-active flag keeps those writes out of the pipe
    // until the pair is validated.
    void hold_writes_until_transport_pair_ready ();
    bool release_writes_for_transport_pair ();

    //  Writes a message to the underlying pipe. Returns false if the
    //  message does not pass check_write. If false, the message object
    //  retains ownership of its message buffer.
    bool write (const msg_t *msg_);

    //  Writes a message assuming HWM was already checked by caller.
    //  Still validates pipe active/termination state.
    bool write_no_hwm_check (const msg_t *msg_);

    // Writes the transport's routing-id setup frame even while a paired
    // Application lane is held for Completion-lane validation.
    bool write_routing_id_and_flush (const msg_t *msg_);
    bool write_transport_probe_and_flush (const msg_t *msg_);

    //  Writes a message and flushes it downstream under the same pipe lock.
    //  Use this for final single-part send hot paths to avoid paying for
    //  separate write/flush lock acquisitions.
    bool write_and_flush (const msg_t *msg_);

    //  Writes a message with the HWM check performed under the already-held
    //  pipe lock without re-entering check_hwm().
    bool write_no_recursive_hwm_check (const msg_t *msg_);

    //  Writes and flushes with the same non-recursive HWM check variant.
    bool write_and_flush_no_recursive_hwm_check (const msg_t *msg_);

    //  Fast path for a single non-routing-id message that is always flushed.
    bool write_single_message_and_flush_no_recursive_hwm_check (const msg_t *msg_);


    //  Remove unfinished parts of the outbound message from the pipe.
    void rollback ();

    //  Flush the messages downstream.
    void flush ();

    //  Temporarily disconnects the inbound message stream and drops
    //  all the messages on the fly. Causes 'hiccuped' event to be generated
    //  in the peer.
    void hiccup ();

    //  Ensure the pipe won't block on receiving pipe_term.
    void set_nodelay ();

    //  Ask pipe to terminate. The termination will happen asynchronously
    //  and user will be notified about actual deallocation by 'terminated'
    //  event. If delay is true, the pending messages will be processed
    //  before actual shutdown.
    void terminate (bool delay_);

    //  Set the high water marks.
    void set_hwms (uint64_t inhwm_, uint64_t outhwm_);
    void set_lwm_hint (uint64_t lwm_hint_);

    //  Set the boost to high water marks, used by inproc sockets so total hwm are sum of connect and bind sockets watermarks
    void set_hwms_boost (uint64_t inhwmboost_, uint64_t outhwmboost_);

    //  Payload bound for a complete message, or 0 when the reader has no
    //  finite bound. The empty-pipe oversize exception is enabled only when
    //  this value is finite.
    void set_max_message_bytes (uint64_t max_message_bytes_);

    // send command to peer for notify the change of hwm
    void send_hwms_to_peer (uint64_t inhwm_, uint64_t outhwm_);

    //  Returns true if HWM is not reached
    bool check_hwm () const;
    //  Checks whether the current multipart transaction can commit with this
    //  frame. A rejected final frame makes the pipe wait for byte credit.
    pipe_message_admission_t check_hwm_for_message (const msg_t *msg_);

    void set_endpoint_pair (endpoint_uri_pair_t endpoint_pair_);
    const endpoint_uri_pair_t &get_endpoint_pair () const;
    void set_transport_connection_id (uint64_t connection_id_);
    uint64_t get_transport_connection_id () const;
    void set_transport_pair (transport_lane_t lane_,
                             uint64_t pair_id_,
                             uint64_t generation_);
    transport_lane_t get_transport_lane () const;
    uint64_t get_transport_pair_id () const;
    uint64_t get_transport_pair_generation () const;
    void set_locally_initiated (bool value_);
    bool is_locally_initiated () const;

    void send_disconnect_msg ();
    void set_disconnect_msg (const std::vector<unsigned char> &disconnect_);

    void send_hiccup_msg (const std::vector<unsigned char> &hiccup_);

    bool retain_lifetime_ref ();
    void release_lifetime_ref ();
    bool has_completed_termination () const;

  private:
    //  Type of the underlying lock-free pipe.
    typedef ypipe_base_t<msg_t> upipe_t;

    //  Command handlers.
    void process_activate_read () ZLINK_OVERRIDE;
    void process_activate_write (uint64_t msgs_read_, uint64_t bytes_read_) ZLINK_OVERRIDE;
    void process_hiccup (void *pipe_) ZLINK_OVERRIDE;
    void process_pipe_term () ZLINK_OVERRIDE;
    void process_pipe_term_ack () ZLINK_OVERRIDE;
    void process_pipe_hwm (uint64_t inhwm_, uint64_t outhwm_) ZLINK_OVERRIDE;

    //  Handler for delimiter read from the pipe.
    void process_delimiter ();

    //  These helpers require `_out_sync` to be held already. They define the
    //  coupled outbound invariants that future `_out_sync` refactors must
    //  preserve: `_out_pipe` lifetime, `_state`, `_out_active`, and
    //  `_peers_msgs_read` move together across write/flush/terminate paths.
    //  enforce_incremental_hwm_ rejects a multipart as soon as the frames
    //  accumulated so far exceed the HWM, instead of waiting for the final
    //  frame. Only writers that check the HWM per call may ask for it: the
    //  *_no_recursive_hwm_check writers admit a whole message up front, so
    //  rejecting one of its later frames would break that guarantee and make
    //  the caller drop a message it was told it could send.
    bool write_message_unlocked (const msg_t *msg_,
                                 bool enforce_hwm_,
                                 bool enforce_incremental_hwm_ = false);
    void rollback_unlocked ();
    void flush_unlocked ();
    uint64_t frame_accounted_bytes (const msg_t *msg_) const;
    bool append_outbound_frame_bytes_unlocked (const msg_t *msg_);
    bool can_commit_bytes_unlocked (uint64_t message_bytes_,
                                    uint64_t payload_bytes_,
                                    bool allow_empty_pipe_exception_) const;
    bool can_commit_bytes_with_peer_snapshot_unlocked (
      uint64_t message_bytes_,
      uint64_t payload_bytes_,
      bool allow_empty_pipe_exception_);
    bool check_hwm_with_peer_snapshot_unlocked ();
    void refresh_peer_credit_snapshot_unlocked ();
    void account_inbound_frame (const msg_t *msg_);

    //  Constructor is private. Pipe can only be created using
    //  pipepair function.
    pipe_t (object_t *parent_,
            upipe_t *inpipe_,
            upipe_t *outpipe_,
            uint64_t inhwm_,
            uint64_t outhwm_,
            bool conflate_,
            bool session_pipe_,
            const std::shared_ptr<transport_lifetime_t> &transport_lifetime_);

    //  Pipepair uses this function to let us know about
    //  the peer pipe object.
    void set_peer (pipe_t *peer_);
    void detach_peer_backref ();

    //  Destructor is private. Pipe objects destroy themselves.
    ~pipe_t () ZLINK_OVERRIDE;

    //  Underlying pipes for both directions.
    //  `_out_pipe`, `_state`, `_out_active`, and `_peers_msgs_read` are a
    //  single outbound state cluster guarded by `_out_sync`.
    upipe_t *_in_pipe;
    upipe_t *_out_pipe;

    //  Can the pipe be read from / written to?
    bool _in_active;
    bool _out_active;
    bool _transport_pair_write_held;
    std::atomic<bool> _waiting_for_byte_credit;

    //  High watermark for the outbound pipe.
    uint64_t _hwm;

    //  Low watermark for the inbound pipe.
    std::atomic<uint64_t> _lwm;
    uint64_t _inhwm;
    uint64_t _lwm_hint;

    // boosts for high and low watermarks, used with inproc sockets so hwm are sum of send and recv hmws on each side of pipe
    uint64_t _in_hwm_boost;
    uint64_t _out_hwm_boost;
    bool _in_hwm_boost_set;
    bool _out_hwm_boost_set;

    //  Number of messages read and written so far.
    uint64_t _msgs_read;
    uint64_t _msgs_written;
    uint64_t _bytes_read;
    uint64_t _bytes_written;
    std::atomic<uint64_t> _published_msgs_read;
    std::atomic<uint64_t> _published_bytes_read;
    uint64_t _last_credit_bytes_read;
    uint64_t _in_incomplete_bytes;
    uint64_t _out_incomplete_bytes;
    uint64_t _out_incomplete_payload_bytes;
    bool _out_multipart_started_empty;
    //  Public payload bound for one complete message, or 0 when unlimited.
    uint64_t _max_message_bytes;
    uint64_t _oversize_message_admission_count;
    uint64_t _oversize_message_admission_max_bytes;
    uint64_t _connected_time;

    //  Last received peer's msgs_read. The actual number in the peer
    //  can be higher at the moment.
    uint64_t _peers_msgs_read;
    uint64_t _peers_bytes_read;

    //  The pipe object on the other side of the pipepair.
    pipe_t *_peer;

    //  Sink to send events to.
    i_pipe_events *_sink;

    //  States of the pipe endpoint:
    //  active: common state before any termination begins,
    //  delimiter_received: delimiter was read from pipe before
    //      term command was received,
    //  waiting_for_delimiter: term command was already received
    //      from the peer but there are still pending messages to read,
    //  term_ack_sent: all pending messages were already read and
    //      all we are waiting for is ack from the peer,
    //  term_req_sent1: 'terminate' was explicitly called by the user,
    //  term_req_sent2: user called 'terminate' and then we've got
    //      term command from the peer as well.
    enum
    {
        active,
        delimiter_received,
        waiting_for_delimiter,
        term_ack_sent,
        term_req_sent1,
        term_req_sent2
    } _state;

    //  If true, we receive all the pending inbound messages before
    //  terminating. If false, we terminate immediately when the peer
    //  asks us to.
    bool _delay;

    //  Routing id of the writer. Used uniquely by the reader side.
    blob_t _router_socket_routing_id;
    blob_t _transport_peer_identity;
    //  Routing id of the writer. Used uniquely by the reader side.
    int _server_socket_routing_id;

    std::atomic<bool> _stream_connect_event_emitted;
    std::atomic<bool> _connection_ready_event_emitted;
    class lifetime_state_t
    {
      public:
        enum transition_t
        {
            transition_invalid,
            transition_complete,
            transition_delete_owner
        };

        lifetime_state_t ();
        bool retain ();
        transition_t release ();
        transition_t complete_termination ();
        bool terminal () const;
        uint32_t refs () const;

      private:
        friend class session_termination_test_access_t;
        static const uint32_t terminal_bit = 0x80000000U;
        static const uint32_t refs_mask = terminal_bit - 1U;
        std::atomic<uint32_t> _state;
    };

    lifetime_state_t _lifetime;
    fast_mutex_t _stream_packet_sync;
    stream_packet_state_t _stream_packet_state;

    //  Returns true if the message is delimiter; false otherwise.
    static bool is_delimiter (const msg_t &msg_);

    //  Computes appropriate low watermark from the given high watermark.
    static uint64_t compute_lwm (uint64_t hwm_);
    static uint64_t apply_lwm_hint (uint64_t hwm_,
                                    uint64_t lwm_,
                                    uint64_t lwm_hint_);
    bool check_hwm_unlocked () const;

    const bool _conflate;

    //  True for session<->socket pipes; hiccup() recreates the inpipe with
    //  the matching (smaller) chunk granularity.
    const bool _session_pipe;

    // The endpoints of this pipe.
    endpoint_uri_pair_t _endpoint_pair;
    std::shared_ptr<transport_lifetime_t> _transport_lifetime;
    transport_lane_t _transport_lane;
    uint64_t _transport_pair_id;
    uint64_t _transport_pair_generation;
    bool _locally_initiated;

    // Disconnect msg
    msg_t _disconnect_msg;
    fast_mutex_t _out_sync;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (pipe_t)
};

void send_routing_id (pipe_t *pipe_, const options_t &options_);

void send_hello_msg (pipe_t *pipe_, const options_t &options_);
}

#endif
