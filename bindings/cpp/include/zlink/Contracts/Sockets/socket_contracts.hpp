/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/context.hpp"
#include "../Messaging/message.hpp"
#include "../Messaging/received.hpp"
#include "../Messaging/subscription_event.hpp"
#include "../Messaging/topic_message.hpp"
#include "../Eventing/events.hpp"
#include "../Eventing/monitor.hpp"
#include "../Core/routing_id.hpp"
#include "socket_options.hpp"
#include "results.hpp"

#include <cerrno>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>


namespace zlink
{

class socket_t;
namespace detail
{
class socket_handle_t;
struct socket_callback_state_t;
struct socket_access_t;
struct recv_envelope_t;
} // namespace detail


class socket_t
{
  public:
    ~socket_t ();
    socket_t (socket_t &&) noexcept;
    socket_t &operator= (socket_t &&) noexcept;
    socket_t (const socket_t &) = delete;
    socket_t &operator= (const socket_t &) = delete;

    bool valid () const noexcept;

    void close ();

    void bind (const std::string &endpoint_);
    void connect (const std::string &endpoint_);
    void unbind (const std::string &endpoint_);
    void disconnect (const std::string &endpoint_);
    void disconnect_rid (const routing_id_t &peer_rid_);

    socket_monitor_t monitor_open (monitor_event events_ = monitor_event::all,
                                   byte_count_t monitor_hwm_bytes_ = byte_count_t::bytes (0)) const
    {
        return socket_monitor_t::open (*this, events_, monitor_hwm_bytes_);
    }

    common_socket_options_t options () { return common_socket_options_t (*this); }

    void set_tls_server (const std::string &cert_,
                         const std::string &key_,
                         bool require_client_cert_ = false);

    void set_tls_client (const std::string &ca_cert_,
                         const std::string &hostname_,
                         bool trust_system_ = false);

    /// @brief Set this socket's local receive-flow state and synchronise it
    /// to the paired DEALER/ROUTER completion lane. Repeating the current
    /// state succeeds as a no-op. Throws @c config_error_t with
    /// @c config_result_t::not_supported for a socket type without a
    /// completion lane (PAIR, PUB/SUB family, STREAM), @c invalid_argument
    /// for a state outside @c receive_flow_state_t, @c invalid_handle for an
    /// invalid socket, and @c invalid_state when a concurrent close wins.
    void set_receive_flow_state (receive_flow_state_t state_);

  protected:
    socket_t () noexcept;

    socket_t (context_t &ctx_, socket_type type_);

    [[nodiscard]] int send (message_t &part_, send_flags_t flags_ = send_flags_t::none);

    [[nodiscard]] int send (std::vector<message_t> &parts_,
                            send_flags_t flags_ = send_flags_t::none);

    [[nodiscard]] int send (const routing_id_t &target_rid_,
                            message_t &part_,
                            send_flags_t flags_ = send_flags_t::none);

    [[nodiscard]] int send (const routing_id_t &target_rid_,
                            std::vector<message_t> &parts_,
                            send_flags_t flags_ = send_flags_t::none);

    [[nodiscard]] int receive (received_t &received_, recv_flags_t flags_ = recv_flags_t::none);

    [[nodiscard]] int
    receive (received_t &received_, recv_flags_t flags_, bool attach_routed_send_context_);

    [[nodiscard]] int receive_impl (
      received_t &received_, recv_flags_t flags_, bool attach_routed_send_context_);

    [[nodiscard]] int publish (const std::string &topic_id_,
                               message_t &part_,
                               send_flags_t flags_ = send_flags_t::none);

    [[nodiscard]] int publish (const std::string &topic_id_,
                               std::vector<message_t> &parts_,
                               send_flags_t flags_ = send_flags_t::none);

  public:
    [[nodiscard]] int set_subscription (const std::string &filter_);
    [[nodiscard]] int unset_subscription (const std::string &filter_);
    [[nodiscard]] int
    subscription_at (size_t index_, std::string &filter_, bool *is_pattern_ = nullptr);

    [[nodiscard]] int subscribe (topic_message_t &message_,
                                 recv_flags_t flags_ = recv_flags_t::none);


    [[nodiscard]] int subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                                      std::string &topic_out_,
                                      message_t &part_out_,
                                      bool &has_more_out_,
                                      recv_flags_t flags_ = recv_flags_t::none);

    [[nodiscard]] int subscription_event (subscription_event_t &event_,
                                          recv_flags_t flags_ = recv_flags_t::none);

  protected:
    detail::socket_callback_state_t &callback_state ();

    [[nodiscard]] int set_routing_id_raw (std::span<const std::byte> data_);

    [[nodiscard]] int set_routing_id_raw (const std::string &routing_id_)
    {
        return set_routing_id_raw (
          std::as_bytes (std::span<const char> (routing_id_.data (), routing_id_.size ())));
    }

    [[nodiscard]] int get_routing_id_raw (routing_id_t &routing_id_) const;

    [[nodiscard]] int get_routing_id_raw (std::string &routing_id_) const
    {
        routing_id_t native_rid = zlink::detail::unchecked_empty_routing_id ();
        if (get_routing_id_raw (native_rid) != 0)
            return -1;
        routing_id_ = native_rid.to_string ();
        return 0;
    }

  private:
    friend struct detail::socket_access_t;

    std::unique_ptr<detail::socket_handle_t> _socket;
    std::shared_ptr<detail::socket_callback_state_t> _callbacks;
    std::unique_ptr<detail::recv_envelope_t> _receive_envelope;
    socket_type _type;
};

} // namespace zlink

namespace zlink
{

void proxy (socket_t &frontend_, socket_t &backend_);
void proxy (socket_t &frontend_, socket_t &backend_, socket_t &capture_);

} // namespace zlink
