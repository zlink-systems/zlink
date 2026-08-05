/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "socket_contracts.hpp"

namespace zlink
{

/// @brief Base for sockets that send and receive multipart messages.
class message_socket_t : public socket_t
{
  protected:
    [[nodiscard]] received_t recv ()
    {
        received_t received;
        const int rc = socket_t::receive (received);
        throw_on_error (rc);
        return received;
    }

    message_socket_t (context_t &ctx_, socket_type type_) : socket_t (ctx_, type_) {}
};

/// @brief Base for sockets that route messages by routing id.
class routed_message_socket_t : public message_socket_t
{
  protected:
    routed_message_socket_t (context_t &ctx_, socket_type type_) : message_socket_t (ctx_, type_) {}
};

} // namespace zlink

namespace zlink
{

/// @brief An exclusive one-to-one peering socket with no routing.
class pair_socket_t : public message_socket_t
{
  public:
    explicit pair_socket_t (context_t &ctx_);

    send_operation_t send ();

    // Receive one message into a caller-provided received_t.
    // Returns 0 on success, a recv_result_t value on receive failure or no data, and -1 only for binding-local failure with errno set.
    int recv (received_t &out_, recv_flags_t flags_ = recv_flags_t::none);

    int recv (message_t &part_out_, recv_flags_t flags_ = recv_flags_t::none);

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

  private:
    using message_socket_t::recv;
};

} // namespace zlink

namespace zlink
{

/// @brief A socket that load-balances sends across its connected peers and can issue routed requests.
class dealer_socket_t : public message_socket_t
{
  public:
    explicit dealer_socket_t (context_t &ctx_);

    send_operation_t send ();

    // Receive one message into a caller-provided received_t.
    // Returns 0 on success, a recv_result_t value on receive failure or no data, and -1 only for binding-local failure with errno set.
    int recv (received_t &out_, recv_flags_t flags_ = recv_flags_t::none);

    int recv (message_t &part_out_, recv_flags_t flags_ = recv_flags_t::none);

    void set_send_ready_handler (std::function<void ()> handler_)
    {
        socket_t::set_send_ready_handler (std::move (handler_));
    }

    request_operation_t request ();

    void set_routing_id (const routing_id_t &routing_id_);

    void get_routing_id (routing_id_t &routing_id_) const;

    dealer_socket_options_t options () { return dealer_socket_options_t (*this); }

  private:
    std::chrono::milliseconds _default_request_timeout;
    using message_socket_t::recv;
};

} // namespace zlink
