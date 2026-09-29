/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DIST_HPP_INCLUDED__
#define __ZLINK_DIST_HPP_INCLUDED__

#include "utils/array.hpp"
#include "utils/macros.hpp"

namespace zlink
{
class pipe_t;
class msg_t;
enum pipe_message_admission_t : int;

//  Class manages a set of outbound pipes. It sends each messages to
//  each of them.
class dist_t
{
  public:
    dist_t ();
    ~dist_t ();

    //  Adds the pipe to the distributor object.
    void attach (zlink::pipe_t *pipe_);

    //  Checks if this pipe is present in the distributor.
    bool has_pipe (zlink::pipe_t *pipe_);

    //  Activates pipe that have previously reached high watermark.
    void activated (zlink::pipe_t *pipe_);

    //  Mark the pipe as matching. Subsequent call to send_to_matching
    //  will send message also to this pipe.
    void match (zlink::pipe_t *pipe_);

    //  Marks all pipes that are not matched as matched and vice-versa.
    void reverse_match ();

    //  Mark all pipes as non-matching.
    void unmatch ();

    //  Removes the pipe from the distributor object.
    void pipe_terminated (zlink::pipe_t *pipe_);

    //  Send the message to the matching outbound pipes.
    int send_to_matching (zlink::msg_t *msg_, bool publish_record_admitted_ = false);

    //  Send the message to all the outbound pipes.
    int send_to_all (zlink::msg_t *msg_);

    //  Select all active pipes before the publish record admission check.
    void match_all ();

    static bool has_out ();

    //  Check HWM of all matching pipes.
    pipe_message_admission_t check_publish_record_hwm (
      bool drop_full_publish_pipes_);

    //  Roll back any queued multipart prefix and restore normal state.
    void rollback ();

  private:
    //  List of outbound pipes.
    typedef array_t<zlink::pipe_t, 2> pipes_t;

    void deactivate_matching_pipe (pipes_t::size_type index_);

    //  Write the message to the pipe. Make the pipe inactive if writing
    //  fails. In such a case false is returned.
    bool write_at (pipes_t::size_type index_, zlink::msg_t *msg_,
                   bool publish_record_admitted_);

    //  Put the message to all active pipes.
    void distribute (zlink::msg_t *msg_, bool publish_record_admitted_);
    pipes_t _pipes;

    //  Number of all the pipes to send the next message to.
    pipes_t::size_type _matching;

    //  Number of active pipes. All the active pipes are located at the
    //  beginning of the pipes array. These are the pipes the messages
    //  can be sent to at the moment.
    pipes_t::size_type _active;

    //  Number of pipes eligible for sending messages to. This includes all
    //  the active pipes plus all the pipes that we can in theory send
    //  messages to (the HWM is not yet reached), but sending a message
    //  to them would result in partial message being delivered, ie. message
    //  with initial parts missing.
    pipes_t::size_type _eligible;

    //  True if last we are in the middle of a multipart message.
    bool _more;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (dist_t)
};
}

#endif
