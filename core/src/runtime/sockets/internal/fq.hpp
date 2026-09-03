/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_FQ_HPP_INCLUDED__
#define __ZLINK_FQ_HPP_INCLUDED__

#include <deque>
#include <set>

#include "utils/array.hpp"
#include "utils/blob.hpp"
#include "core/pipe.hpp"

namespace zlink
{
class msg_t;

//  Class manages a set of inbound pipes. On receive it performs fair
//  queueing so that senders gone berserk won't cause denial of
//  service for decent senders.

class fq_t
{
  public:
    enum receive_activity_publication_t
    {
        keep_receive_activity_local,
        publish_receive_activity
    };

    explicit fq_t (
      receive_activity_publication_t publication_ = keep_receive_activity_local);
    ~fq_t ();

    typedef array_t<pipe_t, 1> pipes_t;

    void attach (pipe_t *pipe_);
    void deactivate (pipe_t *pipe_);
    void activated (pipe_t *pipe_);
    void pipe_terminated (pipe_t *pipe_);
    void arm_dispatch ();

    int recv (msg_t *msg_);
    int recvpipe (msg_t *msg_, pipe_t **pipe_);
    int recvpipe_with_record_admission (
      msg_t *msg_, pipe_t **pipe_, pipe_t::read_admission_fn *admission_,
      void *userdata_);
    bool has_in ();
    bool has_in_with_record_admission (
      pipe_t::read_admission_fn *admission_, void *userdata_);
    size_t redrive_record_admission (size_t max_pipes_);

#ifdef ZLINK_BUILD_TESTS
    typedef bool (*recv_test_hook_fn) (fq_t *fq_, pipe_t *pipe_, void *userdata_);
    static void set_recv_test_hook (recv_test_hook_fn hook_, void *userdata_);
    size_t test_pipe_count () const { return _pipes.size (); }
#endif

  private:
    bool try_get_pipe_index (pipe_t *pipe_, pipes_t::size_type *index_out_);
    template <bool WithAdmission>
    int recvpipe_internal (msg_t *msg_, pipe_t **pipe_,
                           pipe_t::read_admission_fn *admission_,
                           void *userdata_);
    bool block_current_for_record_admission ();
    bool record_admission_blocked (pipe_t *pipe_) const;
    void publish_pipe_receive_activity (pipe_t *pipe_, bool active_) const;
    void normalize_state ();
    pipes_t _pipes;

    //  Number of active pipes. All the active pipes are located at the
    //  beginning of the pipes array.
    pipes_t::size_type _active;

    //  Index of the next bound pipe to read a message from.
    pipes_t::size_type _current;

    //  If true, part of a multipart message was already received, but
    //  there are following parts still waiting in the current pipe.
    bool _more;

    //  A pipe disappeared after exposing a multipart prefix. Surface one
    //  transient receive miss before another pipe may become the source of a
    //  new message, so callers can discard the incomplete record.
    bool _multipart_abort_pending;

    // Only routed count-1 receive paths consume pipe-level FQ activity.
    // Other patterns keep this bookkeeping local to the fair queue.
    const receive_activity_publication_t _receive_activity_publication;

    // Capacity-blocked record heads remain queued on their source pipe. Keep
    // them outside the active FQ partition until the registry owner releases
    // a slot, preserving per-pipe order without letting one source stall
    // unrelated DATA. The deque is the round-robin redrive order; the set
    // suppresses duplicate activation edges while a head remains blocked.
    std::deque<pipe_t *> _record_admission_blocked;
    std::set<pipe_t *> _record_admission_blocked_set;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (fq_t)
};
}

#endif
