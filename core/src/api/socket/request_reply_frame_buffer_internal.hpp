/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_REQUEST_REPLY_FRAME_BUFFER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_REQUEST_REPLY_FRAME_BUFFER_INTERNAL_HPP_INCLUDED__

#include "api/socket/inline_msg_buffer_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
// Keep common application multiparts inline while a request/reply-aware
// receive or completion drain collects one logical message. Only unusually large
// application multiparts need dynamic storage.
const size_t inline_request_reply_frame_capacity = 8;
typedef zlink::socket_internal::inline_msg_buffer_t<inline_request_reply_frame_capacity>
  request_reply_frame_buffer_t;

inline void close_request_reply_frame_buffer (
  request_reply_frame_buffer_t *frames_)
{
    if (!frames_)
        return;

    if (!frames_->empty ())
        zlink::request_reply::consume_send_frames_from (
          frames_->data (), 0, frames_->size ());
    frames_->clear ();
}
}
}

#endif
