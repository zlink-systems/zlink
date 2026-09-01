/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_XSUB_HPP_INCLUDED__
#define __ZLINK_XSUB_HPP_INCLUDED__

#include "sockets/common/socket_base.hpp"
#include "core/session_base.hpp"
#include "sockets/internal/dist.hpp"
#include "sockets/internal/fq.hpp"
#include "core/ctx_physical_queue_registry.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#ifdef ZLINK_USE_RADIX_TREE
#include "utils/radix_tree.hpp"
#else
#include "utils/trie.hpp"
#endif

namespace zlink
{
class ctx_t;
class pipe_t;
class io_thread_t;

class xsub_t : public socket_base_t
{
    friend class session_termination_test_access_t;

  public:
    struct subscription_descriptor_t
    {
        subscription_descriptor_t () : is_pattern (false) {}

        std::string filter;
        bool is_pattern;
    };

    xsub_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~xsub_t () ZLINK_OVERRIDE;
    void snapshot_subscriptions (std::vector<subscription_descriptor_t> *out_) const;

  protected:
    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_,
                       bool locally_initiated_) ZLINK_FINAL;
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_) ZLINK_OVERRIDE;
    int xgetsockopt (int option_, void *optval_, size_t *optvallen_) ZLINK_FINAL;
    int xsend (zlink::msg_t *msg_,
               pipe_message_admission_t *admission_out_ = NULL) ZLINK_OVERRIDE;
    bool xhas_out () ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_) ZLINK_FINAL;
    bool xhas_in () ZLINK_FINAL;
    void xread_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xwrite_activated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    void xhiccuped (pipe_t *pipe_) ZLINK_FINAL;
    void xpipe_terminated (zlink::pipe_t *pipe_) ZLINK_FINAL;
    uint32_t monitor_ready_count () const ZLINK_OVERRIDE;

  private:
    //  Check whether the message matches at least one subscription.
    bool match (zlink::msg_t *msg_);

    //  Function to be applied to the trie to send all the subsciptions
    //  upstream.
    static void send_subscription (unsigned char *data_, size_t size_, void *arg_);
    int discard_filtered_message (zlink::msg_t *msg_, zlink::pipe_t *pipe_);
    void refresh_delivery_ready_state (const endpoint_uri_pair_t &endpoint_uri_pair_);
    uint32_t compute_delivery_ready_count () const;
    bool compute_delivery_ready_state () const;

    //  Fair queueing object for inbound pipes.
    fq_t _fq;

    //  Object for distributing the subscriptions upstream.
    dist_t _dist;

    //  The repository of subscriptions.
#ifdef ZLINK_USE_RADIX_TREE
    radix_tree_t _subscriptions;
#else
    trie_with_size_t _subscriptions;
#endif
    mutable std::mutex _subscriptions_mu;

    // If true, send all unsubscription messages upstream, not just
    // unique ones
    bool _verbose_unsubs;

    //  If true, 'message' contains a matching message to return on the
    //  next recv call.
    bool _has_message;
    msg_t _message;

    //  If true, part of a multipart message was already sent, but
    //  there are following parts still waiting.
    bool _more_send;

    //  If true, part of a multipart message was already received, but
    //  there are following parts still waiting.
    bool _more_recv;
    // Physical part index of the next exposed publication frame. Index 0 is
    // the topic and index 1 is the first caller-visible payload.
    size_t _recv_part_index;
    // A filtered-message probe can discover a malformed continuation before
    // the caller enters xrecv(). Preserve that result for one public receive
    // instead of translating the now-empty queue into EAGAIN.
    bool _recv_protocol_error_pending;

    //  If true, subscribe and cancel messages are processed for the rest
    //  of multipart message.
    bool _process_subscribe;

    //  Bench-aligned SUB steady state subscribes to the empty prefix. In that
    //  state every first frame matches, so we can skip trie lookup on recv.
    std::atomic<bool> _has_empty_subscription;

    std::atomic<uint32_t> _delivery_ready_count;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (xsub_t)
};
}

#endif
