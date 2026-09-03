/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__

#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/part_helper_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
int validate_socket_type (const socket_handle_t &handle_, int expected_type_);
int stage_request_payload_part (zlink::part_helper_internal::handle_state_t *helper_state_,
                                zlink_msg_t *part_);
}
}

#endif
