/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_PENDING_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_PENDING_INTERNAL_HPP_INCLUDED__

#include "api/socket/socket_request_reply_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
int lookup_socket_pending_request_by_seq (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  uint64_t request_seq_,
  pending_key_t *key_out_);
bool erase_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                   const pending_key_t &key_);
void record_socket_pending_transport_pair (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_key_t &key_,
  zlink::pipe_t *transport_pair_pipe_);
int ensure_socket_pending_request (socket_handle_t handle_,
                                   const zlink_routing_id_t *peer_rid_,
                                   uint32_t timeout_ms_,
                                   zlink_reply_handler_fn handler_,
                                   void *userdata_,
                                   uint64_t *request_seq_out_,
                                   std::shared_ptr<socket_request_reply_state_t> *state_out_,
                                   pending_key_t *key_out_);
}
}

#endif
