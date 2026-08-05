/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message_socket_contracts.hpp"

namespace zlink
{

/// @brief Routes messages to peers addressed by routing id; the request/reply server side.
class router_socket_t : public routed_message_socket_t
{
  public:
    using completion_control_handler_t =
      std::function<void (const routing_id_t &, std::vector<message_t>)>;

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

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

    request_operation_t request (const routing_id_t &routing_id_);
    reply_operation_t reply (const routing_id_t &routing_id_, uint64_t request_seq_);

    /// @brief Tries to submit an opaque multipart record on the peer's existing
    /// Completion connection without consuming @p parts_.
    /// @return false only when the Completion connection is backpressured.
    bool try_send_completion_control (const routing_id_t &peer_rid_,
                                      const std::vector<message_t> &parts_);

    /// @brief Installs or replaces the opaque Completion control callback.
    /// The callback owns the received message vector.
    void set_completion_control_handler (completion_control_handler_t handler_);

    void set_routing_id (const routing_id_t &routing_id_);

    void get_routing_id (routing_id_t &routing_id_) const;

    router_socket_options_t options () { return router_socket_options_t (*this); }

  private:
    std::chrono::milliseconds _default_request_timeout;
    using routed_message_socket_t::recv;
};

} // namespace zlink
