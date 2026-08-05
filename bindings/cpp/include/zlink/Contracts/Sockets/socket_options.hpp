/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/byte_count.hpp"
#include "../Core/routing_id.hpp"
#include "../Messaging/message.hpp"

namespace zlink
{

class socket_t;

#if defined(_WIN32)
#if defined(_WIN64)
using socket_fd_t = unsigned long long;
#else
using socket_fd_t = unsigned int;
#endif
#else
using socket_fd_t = int;
#endif

/// @brief TCP keepalive mode for socket options.
enum class tcp_keepalive_mode_t : int
{
    os_default = -1,
    off = 0,
    on = 1
};

/// @brief Determines whether a failed submit is retried.
enum class submit_retry_mode_t : int
{
    off = 0,          ///< Never retry; a failed submit fails immediately.
    local_failure = 1 ///< Retry when the submit fails locally (e.g. back-pressure).
};

/// @brief The typed facade over socket options shared by every socket type.
class common_socket_options_t
{
  public:
    common_socket_options_t () noexcept;
    explicit common_socket_options_t (socket_t &socket_) noexcept;

    std::chrono::milliseconds linger () const;
    void linger (std::chrono::milliseconds value);
    byte_count_t send_hwm () const;
    void send_hwm (byte_count_t value);
    byte_count_t recv_hwm () const;
    void recv_hwm (byte_count_t value);
    std::chrono::milliseconds send_timeout () const;
    void send_timeout (std::chrono::milliseconds value);
    std::chrono::milliseconds recv_timeout () const;
    void recv_timeout (std::chrono::milliseconds value);
    bool immediate () const;
    void immediate (bool value);
    std::chrono::milliseconds connect_timeout () const;
    void connect_timeout (std::chrono::milliseconds value);
    bool ipv6 () const;
    void ipv6 (bool value);
    bool tcp_no_delay () const;
    void tcp_no_delay (bool value);
    tcp_keepalive_mode_t tcp_keepalive () const;
    void tcp_keepalive (tcp_keepalive_mode_t value);
    rid_duplicate_policy_t rid_duplicate_policy () const;
    void rid_duplicate_policy (rid_duplicate_policy_t value);
    byte_size_t max_message_size () const;
    void max_message_size (byte_size_t value);
    socket_backlog_t backlog () const;
    void backlog (socket_backlog_t value);
    std::chrono::milliseconds reconnect_interval () const;
    void reconnect_interval (std::chrono::milliseconds value);
    std::chrono::milliseconds reconnect_interval_max () const;
    void reconnect_interval_max (std::chrono::milliseconds value);
    submit_retry_mode_t submit_retry_mode () const;
    void submit_retry_mode (submit_retry_mode_t value);
    std::chrono::milliseconds submit_retry_timeout () const;
    void submit_retry_timeout (std::chrono::milliseconds value);
    int submit_retry_attempts () const;
    void submit_retry_attempts (int value);
    std::string last_endpoint () const;

  protected:
    socket_t *_socket;
};

/// @brief The typed facade over ROUTER-specific socket options.
class router_socket_options_t : public common_socket_options_t
{
  public:
    router_socket_options_t () noexcept;
    explicit router_socket_options_t (socket_t &socket_) noexcept;

    bool mandatory () const;
    void mandatory (bool value);
    bool handover () const;
    void handover (bool value);
    bool probe () const;
    void probe (bool value);
    std::optional<routing_id_t> connect_routing_id () const;
    void connect_routing_id (const routing_id_t &value);
    std::chrono::milliseconds request_timeout () const;
    void request_timeout (std::chrono::milliseconds value);
    peer_weight_t peer_weight () const;
    void peer_weight (peer_weight_t value);
};

/// @brief The typed facade over DEALER-specific socket options.
class dealer_socket_options_t : public common_socket_options_t
{
  public:
    dealer_socket_options_t () noexcept;
    explicit dealer_socket_options_t (socket_t &socket_) noexcept;

    bool probe () const;
    void probe (bool value);
    std::chrono::milliseconds request_timeout () const;
    void request_timeout (std::chrono::milliseconds value);
    peer_weight_t peer_weight () const;
    void peer_weight (peer_weight_t value);
};

/// @brief The typed facade over STREAM-specific socket options.
class stream_socket_options_t : public common_socket_options_t
{
  public:
    stream_socket_options_t () noexcept;
    explicit stream_socket_options_t (socket_t &socket_) noexcept;

    bool notify () const;
    void notify (bool value);
};

/// @brief The typed facade over PUB/XPUB-specific socket options.
class pub_socket_options_t : public common_socket_options_t
{
  public:
    pub_socket_options_t () noexcept;
    explicit pub_socket_options_t (socket_t &socket_) noexcept;

    bool verbose () const;
    void verbose (bool value);
    bool verboser () const;
    void verboser (bool value);
    bool no_drop () const;
    void no_drop (bool value);
    bool manual () const;
    void manual (bool value);
    bool manual_last_value () const;
    void manual_last_value (bool value);
    message_t welcome_message () const;
    void welcome_message (const message_t &message);
    void approve_subscribe (const routing_id_t &routing_id);
    void reject_subscribe (const routing_id_t &routing_id);
    int topics_count () const;
};

/// @brief The typed facade over SUB/XSUB-specific socket options.
class sub_socket_options_t : public common_socket_options_t
{
  public:
    sub_socket_options_t () noexcept;
    explicit sub_socket_options_t (socket_t &socket_) noexcept;

    int topics_count () const;
};

} // namespace zlink
