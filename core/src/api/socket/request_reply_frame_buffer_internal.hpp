/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_REPLY_FRAME_BUFFER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_REPLY_FRAME_BUFFER_INTERNAL_HPP_INCLUDED__

#include "api/socket/inline_msg_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
// A normal request/reply record is four control frames plus one or two
// payload frames. Keep that aggregate in the receiving call; only unusually
// large multipart records need dynamic storage.
const size_t inline_request_reply_frame_capacity = 8;
typedef zlink::socket_internal::inline_msg_buffer_t<inline_request_reply_frame_capacity>
  request_reply_frame_buffer_t;

inline void close_request_reply_frame_buffer (
  request_reply_frame_buffer_t *frames_)
{
    if (!frames_)
        return;

    if (!frames_->empty ())
        zlink::request_reply::close_built_parts (frames_->data (), frames_->size ());
    frames_->clear ();
}
}

inline void close_msg_frames (
  socket_reqrep_internal::request_reply_frame_buffer_t *frames_)
{
    socket_reqrep_internal::close_request_reply_frame_buffer (frames_);
}
}

#endif
