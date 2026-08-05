/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DEALER_HPP_INCLUDED__
#define __ZLINK_DEALER_HPP_INCLUDED__

#include "sockets/common/socket_base.hpp"
#include "core/session_base.hpp"
#include "sockets/internal/fq.hpp"
#include "sockets/internal/lb.hpp"
#include <map>
#include <vector>

namespace zlink
{
class ctx_t;
class msg_t;
class pipe_t;
class io_thread_t;
class socket_base_t;

class dealer_t : public socket_base_t
{
  public:
    dealer_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~dealer_t () ZLINK_OVERRIDE;

    int sendpipe_to (zlink::pipe_t *pipe_, zlink::msg_t *msg_, int flags_);

  protected:
    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_OVERRIDE;
    int xgetsockopt (int option_, void *optval_, size_t *optvallen_) ZLINK_OVERRIDE;
    int xsend (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xsend_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_OVERRIDE;
    int xrecv_pipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    bool xhas_in () ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    int xrollback () ZLINK_OVERRIDE;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xwrite_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    int xsocket_msg_dispatch (zlink::msg_t *msg_, zlink::pipe_t *pipe_) ZLINK_OVERRIDE;
    void xarm_socket_msg_dispatch () ZLINK_OVERRIDE;
    void xdispatch_io () ZLINK_OVERRIDE;

    //  Send and recv - knowing which pipe was used.
    int sendpipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_);
    int recvpipe (zlink::msg_t *msg_, zlink::pipe_t **pipe_);

  private:
    int apply_peer_weight (pipe_t *pipe_, uint32_t weight_) ZLINK_OVERRIDE;

    //  Messages are fair-queued from inbound pipes. And load-balanced to
    //  the outbound pipes.
    fq_t _fq;
    lb_t _lb;

    // if true, send an empty message to every connected router peer
    bool _probe_router;
    std::vector<zlink_msg_t> _dispatch_parts;
    std::map<zlink::pipe_t *, std::vector<zlink_msg_t>> _dispatch_parts_by_pipe;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (dealer_t)
};
}

#endif
