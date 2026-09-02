/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message_socket_contracts.hpp"

namespace zlink
{

/// @brief Routes messages to peers addressed by routing id; the request/reply server side.
class router_socket_t : public routed_message_socket_t
{
  public:
    explicit router_socket_t (context_t &ctx_);

    send_operation_t send (const routing_id_t &target_rid_);

    // Receive one message into a caller-provided received_t.
    // Returns 0 on success, a recv_result_t value on receive failure or no data, and -1 only for binding-local failure with errno set. The caller may keep a long-lived received_t
    // across recv calls so that the parts vector / routing id storage is
    // reused without reallocation.
    int recv (received_t &out_, recv_flags_t flags_ = recv_flags_t::none);

    int recv (routing_id_t &source_rid_out_,
              message_t &part_out_,
              recv_flags_t flags_ = recv_flags_t::none);

    request_operation_t request (const routing_id_t &routing_id_);
    reply_operation_t reply (const routing_id_t &routing_id_, reply_token_t reply_token_);

    void set_routing_id (const routing_id_t &routing_id_);

    void get_routing_id (routing_id_t &routing_id_) const;

    router_socket_options_t options () { return router_socket_options_t (*this); }

  private:
    std::chrono::milliseconds _default_request_timeout;
    using routed_message_socket_t::recv;
};

} // namespace zlink
