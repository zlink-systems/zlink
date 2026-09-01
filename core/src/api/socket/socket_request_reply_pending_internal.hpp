/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_PENDING_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_PENDING_INTERNAL_HPP_INCLUDED__

#include "api/socket/socket_request_reply_internal.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
int lookup_socket_pending_request (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_identity_t &identity_,
  pending_request_token_t *token_out_);
bool erase_socket_pending_request (const std::shared_ptr<socket_request_reply_state_t> &state_,
                                   const pending_request_identity_t &identity_);
bool record_socket_pending_transport_pair_identity (
  const std::shared_ptr<socket_request_reply_state_t> &state_,
  const pending_request_identity_t &identity_,
  uint64_t transport_pair_id_,
  uint64_t transport_pair_generation_);
}
}

#endif
