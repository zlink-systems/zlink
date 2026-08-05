/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message_socket_contracts.hpp"
#include <vector>

namespace zlink
{

/// @brief Exchanges framed packets with raw TCP peers.
class stream_socket_t : public routed_message_socket_t
{
  public:
    explicit stream_socket_t (context_t &ctx_);

    send_operation_t send (const routing_id_t &target_rid_);

    // Receive one message into a caller-provided received_t.
    // Returns 0 on success, a recv_result_t value on receive failure or no data, and -1 only for binding-local failure with errno set.
    int recv (received_t &out_, recv_flags_t flags_ = recv_flags_t::none);

    void set_packet_handler (
      std::function<void (const routing_id_t &, message_t &&, message_t &&)> handler_);

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

    void set_routing_id (const routing_id_t &routing_id_);

    void get_routing_id (routing_id_t &routing_id_) const;

    stream_socket_options_t options () { return stream_socket_options_t (*this); }

  private:
    using routed_message_socket_t::recv;
    using socket_t::connect;
    using socket_t::disconnect;
    using socket_t::disconnect_rid;
};

} // namespace zlink
