/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_LB_HPP_INCLUDED__
#define __ZLINK_LB_HPP_INCLUDED__

#include <map>
#include <vector>

#include "utils/array.hpp"

namespace zlink
{
class msg_t;
class pipe_t;

//  This class manages a set of outbound pipes. On send it load balances
//  messages fairly among the pipes.

class lb_t
{
  public:
    lb_t ();
    ~lb_t ();

    void attach (pipe_t *pipe_);
    void activated (pipe_t *pipe_);
    void pipe_terminated (pipe_t *pipe_);
    void set_weight (pipe_t *pipe_, uint32_t weight_);
    uint32_t weight (pipe_t *pipe_) const;
    bool has_positive_weight_pipe () const;
    bool contains (pipe_t *pipe_) const;

    int send (msg_t *msg_);

    //  Sends a message and stores the pipe that was used in pipe_.
    //  It is possible for this function to return success but keep pipe_
    //  unset if the rest of a multipart message to a terminated pipe is
    //  being dropped. For the first frame, this will never happen.
    int sendpipe (msg_t *msg_, pipe_t **pipe_);

    //  Removes an unfinished multipart message and resets send sequencing.
    void rollback ();

    bool has_out ();

  private:
    //  List of outbound pipes.
    typedef array_t<pipe_t, 2> pipes_t;
    pipes_t _pipes;

    //  Per-pipe routing state. The running value implements the smooth
    //  weighted selection procedure defined by the DEALER spec: every
    //  selection adds each candidate's weight to its running value, picks the
    //  largest one and subtracts the total weight from the winner.
    struct pipe_entry_t
    {
        pipe_entry_t () : weight (100), running_value (0), attach_seq (0) {}

        uint32_t weight;
        int64_t running_value;
        uint64_t attach_seq;
    };
    typedef std::map<pipe_t *, pipe_entry_t> entries_t;

    //  One active candidate. The entry pointer keeps the send path off the
    //  map: std::map never invalidates references to elements it did not
    //  erase, and the only erase site marks the order dirty, so the vector is
    //  always rebuilt before a stale pointer could be read.
    struct candidate_t
    {
        candidate_t () : pipe (NULL), entry (NULL) {}
        candidate_t (pipe_t *pipe_, pipe_entry_t *entry_) : pipe (pipe_), entry (entry_) {}

        pipe_t *pipe;
        pipe_entry_t *entry;
    };

    //  Orders candidates by the stable identifier used for tie-breaking:
    //  peer routing ID first, then the endpoint identifier, then the
    //  process-local attach order as a last resort.
    struct candidate_order_t
    {
        bool operator() (const candidate_t &lhs_, const candidate_t &rhs_) const;
    };

    //  Number of active pipes. All the active pipes are located at the
    //  beginning of the pipes array.
    pipes_t::size_type _active;

    //  Points to the last pipe that the most recent message was sent to.
    pipes_t::size_type _current;

    //  True if last we are in the middle of a multipart message.
    bool _more;

    //  True if we are dropping current message.
    bool _dropping;
    entries_t _entries;
    uint64_t _attach_seq;
    bool _order_dirty;

    //  Active candidates in ascending identifier order.
    std::vector<candidate_t> _ordered;
    pipe_t *_weighted_multipart_pipe;

    void deactivate (pipe_t *pipe_);
    void mark_selection_dirty ();
    void rebuild_selection_order ();

    //  Returns the candidate the selection procedure picks next without
    //  mutating any running value, and reports the total candidate weight the
    //  winner has to repay once the write succeeds. The result points into
    //  _ordered and is valid until the order is rebuilt.
    const candidate_t *select_weighted_pipe (uint32_t *total_weight_out_);

    //  Applies one selection step for a write that actually happened.
    void commit_weighted_selection (pipe_entry_t *selected_, uint32_t total_weight_);

    ZLINK_NON_COPYABLE_NOR_MOVABLE (lb_t)
};
}

#endif
