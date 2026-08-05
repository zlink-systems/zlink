/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ROUTER_HPP_INCLUDED__
#define __ZLINK_ROUTER_HPP_INCLUDED__

#include <map>
#include <vector>

#include "sockets/common/socket_base.hpp"
#include "core/session_base.hpp"
#include "utils/stdint.hpp"
#include "utils/blob.hpp"
#include "core/msg.hpp"
#include "sockets/internal/fq.hpp"

namespace zlink
{
class ctx_t;
class pipe_t;

//  Outbound eligibility is tracked through routing-id lookup and cached
//  writable state to avoid full pipe scans on the hot path.
class router_t : public routing_socket_base_t
{
  public:
    router_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~router_t () ZLINK_OVERRIDE;

    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_FINAL;
    int xgetsockopt (int option_, void *optval_, size_t *optvallen_) ZLINK_FINAL;
    int xsend (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xsend_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    int xsend_routed (const zlink_routing_id_t *target_rid_,
                      zlink::msg_t *msg_,
                      uint64_t *connection_id_out_,
                      uint64_t expected_connection_id_,
                      zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xrecv_routed (zlink::msg_t *msg_,
                      zlink_routing_id_t *source_rid_out_,
                      uint64_t *connection_id_out_,
                      zlink::pipe_t **source_pipe_out_ = NULL) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    int xsocket_msg_dispatch (zlink::msg_t *msg_, zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    int xterm_peer_rid (const zlink_routing_id_t *peer_rid_) ZLINK_OVERRIDE
    {
        return terminate_out_pipe_by_routing_id (peer_rid_);
    }
    void xarm_socket_msg_dispatch () ZLINK_OVERRIDE;
    void xdispatch_io () ZLINK_OVERRIDE;
    int get_peer_state (const void *routing_id_, size_t routing_id_size_) const ZLINK_FINAL;

  protected:
    //  Rollback any message parts that were sent but not yet flushed.
    int xrollback () ZLINK_OVERRIDE;

  private:
    //  Receive peer id and update lookup map
    bool identify_peer (pipe_t *pipe_, bool locally_initiated_);
    bool adopt_peer_routing_id (pipe_t *pipe_, blob_t routing_id_, bool locally_initiated_);
    bool duplicate_pipe_should_replace (const out_pipe_t &existing_outpipe_,
                                        const blob_t &routing_id_,
                                        bool locally_initiated_) const;
    void copy_router_pipe_source_rid (pipe_t *pipe_,
                                      zlink_routing_id_t *out_) const;
    void promote_anonymous_pipe_for_dispatch (pipe_t *pipe_);
    int apply_peer_weight (pipe_t *pipe_, uint32_t weight_) ZLINK_OVERRIDE;

    //  Fair queueing object for inbound pipes.
    fq_t _fq;

    //  True iff there is a message held in the pre-fetch buffer.
    bool _prefetched;

    //  If true, the receiver got the message part with
    //  the peer's identity.
    bool _routing_id_sent;

    //  Holds the prefetched identity.
    msg_t _prefetched_id;

    //  Holds the prefetched message.
    msg_t _prefetched_msg;

    //  The pipe we are currently reading from
    zlink::pipe_t *_current_in;

    //  Should current_in should be terminate after all parts received?
    bool _terminate_current_in;

    //  If true, more incoming message parts are expected.
    bool _more_in;

    //  Pipes awaiting a peer routing id retain whether this socket initiated
    //  the transport. Duplicate admission needs that direction after the
    //  asynchronous handshake completes.
    std::map<pipe_t *, bool> _anonymous_pipes;

    //  Reciprocal connectors can establish two physical pipes for one stable
    //  routing id. The deterministic loser stays attached under an internal
    //  id instead of being terminated into a reconnect loop. If the selected
    //  pipe closes, the existing standby is promoted immediately.
    std::map<pipe_t *, blob_t> _standby_pipes;

    //  The pipe we are currently writing to.
    zlink::pipe_t *_current_out;
    uint64_t _current_out_connection_id;

    //  If true, more outgoing message parts are expected.
    bool _more_out;

    //  Routing IDs are generated. It's a simple increment and wrap-over
    //  algorithm. This value is the next ID to use (if not used already).
    uint32_t _next_integral_routing_id;

    // If true, report EAGAIN to the caller instead of silently dropping
    // the message targeting an unknown peer.
    bool _mandatory;
    // if true, send an empty message to every connected router peer
    bool _probe_router;

    // If true, the router will reassign an identity upon encountering a
    // name collision. The selected pipe takes the identity.
    bool _handover;
    std::vector<zlink_msg_t> _dispatch_parts;
    std::map<pipe_t *, std::vector<zlink_msg_t>> _dispatch_parts_by_pipe;
    std::map<pipe_t *, zlink_routing_id_t> _dispatch_source_rids;
    zlink_routing_id_t _dispatch_source_rid;
    bool _dispatch_source_rid_valid;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (router_t)
};
}

#endif
