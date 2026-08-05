/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_SUBMIT_INTERNAL_HPP_INCLUDED__

#include "api/socket/socket_request_reply_pending_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/part_helper_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
int validate_socket_type (void *socket_, int expected_type_);
int validate_request_send_flags (zlink_send_flags_t flags_);
int send_request_frame (zlink::socket_base_t *socket_,
                        zlink::part_helper_internal::handle_state_t *helper_state_,
                        const zlink_routing_id_t *peer_rid_,
                        const void *data_,
                        size_t size_,
                        int flags_,
                        zlink::pipe_t **application_pipe_out_ = NULL);
int send_request_payload_part (zlink::socket_base_t *socket_,
                               zlink::part_helper_internal::handle_state_t *helper_state_,
                               const zlink_routing_id_t *peer_rid_,
                               zlink_msg_t *part_,
                               zlink_send_flags_t flags_,
                               zlink_part_flag_t part_flag_);
int stage_request_payload_part (zlink::part_helper_internal::handle_state_t *helper_state_,
                                zlink_msg_t *part_);
}
}

#endif
