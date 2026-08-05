/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_PERF_SOCKET_ADAPTER_HPP
#define ZLINK_CPP_PERF_SOCKET_ADAPTER_HPP

#include "perf_socket_options_adapter.hpp"

#include <zlink.hpp>

#include <cerrno>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace perf
{

using namespace zlink;

class socket_t
{
  private:
    using socket_variant_t = std::variant<std::monostate,
                                          pair_socket_t,
                                          dealer_socket_t,
                                          router_socket_t,
                                          pub_socket_t,
                                          sub_socket_t,
                                          xpub_socket_t,
                                          stream_socket_t>;

  public:
    socket_t () noexcept : _socket (), _type (socket_type::pair) {}

    socket_t (context_t &ctx_, socket_type type_) : _socket (), _type (type_)
    {
        switch (type_) {
            case socket_type::pair:
                _socket.emplace<pair_socket_t> (ctx_);
                break;
            case socket_type::dealer:
                _socket.emplace<dealer_socket_t> (ctx_);
                break;
            case socket_type::router:
                _socket.emplace<router_socket_t> (ctx_);
                break;
            case socket_type::pub:
                _socket.emplace<pub_socket_t> (ctx_);
                break;
            case socket_type::sub:
                _socket.emplace<sub_socket_t> (ctx_);
                break;
            case socket_type::xpub:
                _socket.emplace<xpub_socket_t> (ctx_);
                break;
            case socket_type::stream:
                _socket.emplace<stream_socket_t> (ctx_);
                break;
            default:
                errno = EOPNOTSUPP;
                break;
        }
    }

    bool valid () const noexcept
    {
        return visit_const ([] (const auto &socket_) {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value)
                return false;
            else
                return socket_.valid ();
        });
    }

    int bind (const std::string &endpoint_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                errno = ENOTSOCK;
                return -1;
            } else {
                try {
                    socket_.bind (endpoint_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            }
        });
    }

    int connect (const std::string &endpoint_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, dealer_socket_t>::value
                          || std::is_same<socket_type_t, pair_socket_t>::value
                          || std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, pub_socket_t>::value
                          || std::is_same<socket_type_t, sub_socket_t>::value
                          || std::is_same<socket_type_t, xpub_socket_t>::value) {
                try {
                    socket_.connect (endpoint_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    socket_monitor_t monitor_open (monitor_event events_ = monitor_event::all) const
    {
        return visit_const ([&] (const auto &socket_) -> socket_monitor_t {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                throw config_error_t (config_result_t::invalid_handle, ENOTSOCK);
            } else {
                return socket_.monitor_open (events_);
            }
        });
    }

    void poller_add (poller_t &poller_, poll_event_flag_t events_, std::uintptr_t slot_ = 0)
    {
        visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                throw config_error_t (config_result_t::invalid_handle, ENOTSOCK);
            } else {
                poller_.add (socket_, events_, slot_);
                return 0;
            }
        });
    }

    void poller_modify (poller_t &poller_, poll_event_flag_t events_)
    {
        visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                throw config_error_t (config_result_t::invalid_handle, ENOTSOCK);
            } else {
                poller_.modify (socket_, events_);
                return 0;
            }
        });
    }

    int disconnect (const std::string &endpoint_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, dealer_socket_t>::value
                          || std::is_same<socket_type_t, pair_socket_t>::value
                          || std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, pub_socket_t>::value
                          || std::is_same<socket_type_t, sub_socket_t>::value
                          || std::is_same<socket_type_t, xpub_socket_t>::value) {
                try {
                    socket_.disconnect (endpoint_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int set_option (socket_option_key_t<T> key_, const T &value_)
    {
        return invoke_common_set (key_.option, value_);
    }

    int set_option (socket_option_key_t<std::string> key_, const std::string &value_)
    {
        return invoke_common_set (key_.option, value_);
    }

    template <typename T> int set (socket_option_key_t<T> key_, const T &value_)
    {
        return set_option (key_, value_);
    }

    template <typename T> int set (router_option_key_t<T> key_, const T &value_)
    {
        return invoke_router_set (key_.option, value_);
    }

    template <typename T> int set (dealer_option_key_t<T> key_, const T &value_)
    {
        return invoke_dealer_set (key_.option, value_);
    }

    template <typename T> int set (pub_option_key_t<T> key_, const T &value_)
    {
        return invoke_pub_set (key_.option, value_);
    }

    template <typename T> int set (sub_option_key_t<T> key_, const T &value_)
    {
        return invoke_sub_set (key_.option, value_);
    }

    template <typename T> int set (stream_option_key_t<T> key_, const T &value_)
    {
        return invoke_stream_set (key_.option, value_);
    }

    template <typename T> int get_option (socket_option_key_t<T> key_, T *value_) const
    {
        if (!value_) {
            errno = EINVAL;
            return -1;
        }
        return invoke_common_get (key_.option, *value_);
    }

    int get_option (socket_option_key_t<std::string> key_, std::string &value_) const
    {
        return invoke_common_get (key_.option, value_);
    }

    template <typename T> int get (socket_option_key_t<T> key_, T &value_) const
    {
        return get_option (key_, &value_);
    }

    int get (socket_option_key_t<std::string> key_, std::string &value_) const
    {
        return get_option (key_, value_);
    }

    template <typename T> int get (router_option_key_t<T> key_, T &value_) const
    {
        return invoke_router_get (key_.option, value_);
    }

    template <typename T> int get (pub_option_key_t<T> key_, T &value_) const
    {
        return invoke_pub_get (key_.option, value_);
    }

    int set_routing_id (const void *data_, size_t size_)
    {
        try {
            routing_id_t routing_id =
              routing_id_t::from (static_cast<const uint8_t *> (data_), size_);
            return invoke_routing_id_set (routing_id);
        }
        catch (const std::invalid_argument &) {
            errno = size_ > 255u ? EMSGSIZE : EINVAL;
            return -1;
        }
    }

    int set_routing_id (const std::string &routing_id_)
    {
        try {
            routing_id_t routing_id = routing_id_t::from (
              reinterpret_cast<const uint8_t *> (routing_id_.data ()), routing_id_.size ());
            return invoke_routing_id_set (routing_id);
        }
        catch (const std::invalid_argument &) {
            errno = routing_id_.size () > 255u ? EMSGSIZE : EINVAL;
            return -1;
        }
    }

    int set_tls_server (const std::string &cert_,
                        const std::string &key_,
                        bool require_client_cert_ = false)
    {
        return invoke_void (
          [&] (auto &socket_) { socket_.set_tls_server (cert_, key_, require_client_cert_); });
    }

    int set_tls_client (const std::string &ca_cert_,
                        const std::string &hostname_,
                        bool trust_system_ = false)
    {
        return invoke_void (
          [&] (auto &socket_) { socket_.set_tls_client (ca_cert_, hostname_, trust_system_); });
    }

    int send (message_t &part_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pair_socket_t>::value
                          || std::is_same<socket_type_t, dealer_socket_t>::value) {
                try {
                    const bool sent =
                      std::move (socket_.send ()).message (part_).flags (flags_).submit ();
                    return bool_result_to_errno (sent);
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int send (const routing_id_t &routing_id_, message_t &part_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, stream_socket_t>::value) {
                try {
                    const bool sent = std::move (socket_.send (routing_id_))
                                        .message (part_)
                                        .flags (flags_)
                                        .submit ();
                    return bool_result_to_errno (sent);
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int publish (const std::string &topic_id_, message_t &part_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pub_socket_t>::value
                          || std::is_same<socket_type_t, xpub_socket_t>::value) {
                try {
                    const bool sent = std::move (socket_.publish (topic_id_))
                                        .message (part_)
                                        .flags (flags_)
                                        .submit ();
                    return bool_result_to_errno (sent);
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int recv (message_t &part_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pair_socket_t>::value
                          || std::is_same<socket_type_t, dealer_socket_t>::value) {
                return recv_single_part_impl (socket_, NULL, part_, flags_);
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int recv (routing_id_t &source_rid_out_, message_t &part_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, stream_socket_t>::value) {
                return recv_single_part_impl (socket_, &source_rid_out_, part_, flags_);
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int receive (received_t &received_out_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pair_socket_t>::value
                          || std::is_same<socket_type_t, dealer_socket_t>::value
                          || std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, stream_socket_t>::value) {
                return recv_received_impl (socket_, received_out_, flags_);
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    bool request (message_t &part_,
                  std::chrono::milliseconds timeout_,
                  int flags_,
                  request_callback_t callback_)
    {
        return visit ([&] (auto &socket_) -> bool {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, dealer_socket_t>::value) {
                return std::move (socket_.request ())
                  .message (part_)
                  .timeout (timeout_)
                  .flags (flags_)
                  .submit (std::move (callback_));
            } else {
                errno = EOPNOTSUPP;
                return false;
            }
        });
    }

    bool request (const routing_id_t &target_rid_,
                  message_t &part_,
                  std::chrono::milliseconds timeout_,
                  int flags_,
                  request_callback_t callback_)
    {
        return visit ([&] (auto &socket_) -> bool {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value) {
                return std::move (socket_.request (target_rid_))
                  .message (part_)
                  .timeout (timeout_)
                  .flags (flags_)
                  .submit (std::move (callback_));
            } else {
                errno = EOPNOTSUPP;
                return false;
            }
        });
    }

    int
    try_send_result (send_result_t &result_, const routing_id_t &target_rid_, message_t &part_)
    {
        result_ = send_result_t::sent;
        try {
            const int rc =
              send (target_rid_, part_, static_cast<int> (zlink::send_flags_t::dontwait));
            if (rc == 0)
                return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                result_ = send_result_t::backpressured;
                return 0;
            }
            return -1;
        }
        catch (const submit_error_t &err) {
            errno = err.internal_errno ();
            if (err.result () == submit_result_t::not_connected
                || err.result () == submit_result_t::not_found) {
                result_ = send_result_t::not_ready;
                return 0;
            }
            if (err.result () == submit_result_t::backpressured) {
                result_ = send_result_t::backpressured;
                return 0;
            }
            return -1;
        }
    }

    int
    set_packet_handler (std::function<void (const routing_id_t &, message_t, message_t)> handler_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, stream_socket_t>::value) {
                try {
                    socket_.set_packet_handler (std::move (handler_));
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int set_subscription (const std::string &filter_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, sub_socket_t>::value) {
                try {
                    socket_.set_subscription (filter_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int subscribe (topic_message_t &topic_message_out_, int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, sub_socket_t>::value) {
                try {
                    const int rc =
                      socket_.subscribe (topic_message_out_, static_cast<recv_flags_t> (flags_));
                    if (rc == static_cast<int> (recv_result_t::ok))
                        return 0;
                    if (rc == static_cast<int> (recv_result_t::no_data)) {
                        errno = EAGAIN;
                        return -1;
                    }
                    errno = EIO;
                    return -1;
                }
                catch (const recv_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int subscribe_part (std::optional<routing_id_t> &source_rid_out_,
                        std::string &topic_out_,
                        message_t &part_out_,
                        bool &has_more_out_,
                        int flags_ = 0)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, sub_socket_t>::value) {
                try {
                    const int rc =
                      socket_.subscribe_part (source_rid_out_, topic_out_, part_out_, has_more_out_,
                                              static_cast<recv_flags_t> (flags_));
                    if (rc == static_cast<int> (recv_result_t::ok))
                        return 0;
                    if (rc == static_cast<int> (recv_result_t::no_data)) {
                        errno = EAGAIN;
                        return -1;
                    }
                    errno = EIO;
                    return -1;
                }
                catch (const recv_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

  private:
    template <typename Fn>
    auto visit (Fn &&fn) -> decltype (std::visit (std::forward<Fn> (fn),
                                                  std::declval<socket_variant_t &> ()))
    {
        return std::visit (std::forward<Fn> (fn), _socket);
    }

    template <typename Fn>
    auto visit_const (Fn &&fn) const
      -> decltype (std::visit (std::forward<Fn> (fn), std::declval<const socket_variant_t &> ()))
    {
        return std::visit (std::forward<Fn> (fn), _socket);
    }

    template <typename Fn> int invoke_void (Fn fn)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                errno = ENOTSOCK;
                return -1;
            } else {
                try {
                    fn (socket_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            }
        });
    }

    template <typename T> int invoke_common_set (socket_option option_, const T &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                errno = ENOTSOCK;
                return -1;
            } else {
                try {
                    common_socket_options_t options = socket_.options ();
                    switch (option_) {
                        case socket_option::linger:
                            options.linger (std::chrono::milliseconds (static_cast<int> (value_)));
                            return 0;
                        case socket_option::sndhwm:
                            options.send_hwm (byte_count_t::bytes (value_));
                            return 0;
                        case socket_option::rcvhwm:
                            options.recv_hwm (byte_count_t::bytes (value_));
                            return 0;
                        case socket_option::sndtimeo:
                            options.send_timeout (
                              std::chrono::milliseconds (static_cast<int> (value_)));
                            return 0;
                        case socket_option::rcvtimeo:
                            options.recv_timeout (
                              std::chrono::milliseconds (static_cast<int> (value_)));
                            return 0;
                        case socket_option::tcp_nodelay:
                            options.tcp_no_delay (static_cast<int> (value_) != 0);
                            return 0;
                        case socket_option::connect_timeout:
                            options.connect_timeout (
                              std::chrono::milliseconds (static_cast<int> (value_)));
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            }
        });
    }

    int invoke_common_set (socket_option option_, const std::string &value_)
    {
        (void) option_;
        (void) value_;
        errno = EOPNOTSUPP;
        return -1;
    }

    template <typename T> int invoke_common_get (socket_option option_, T &value_) const
    {
        return visit_const ([&] (const auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                errno = ENOTSOCK;
                return -1;
            } else {
                try {
                    switch (option_) {
                        case socket_option::type:
                            value_ = static_cast<T> (_type);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            }
        });
    }

    int invoke_common_get (socket_option option_, std::string &value_) const
    {
        return visit_const ([&] (const auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, std::monostate>::value) {
                errno = ENOTSOCK;
                return -1;
            } else {
                try {
                    switch (option_) {
                        case socket_option::last_endpoint:
                            value_ =
                              const_cast<socket_type_t &> (socket_).options ().last_endpoint ();
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            }
        });
    }

    template <typename T> int invoke_router_set (router_option option_, const T &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value) {
                try {
                    router_socket_options_t options = socket_.options ();
                    switch (option_) {
                        case router_option::mandatory:
                            options.mandatory (static_cast<int> (value_) != 0);
                            return 0;
                        case router_option::probe:
                            options.probe (static_cast<int> (value_) != 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int invoke_router_set (router_option option_, const std::string &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value) {
                try {
                    if (option_ != router_option::connect_routing_id) {
                        errno = EOPNOTSUPP;
                        return -1;
                    }
                    routing_id_t routing_id = routing_id_t::from (
                      reinterpret_cast<const uint8_t *> (value_.data ()), value_.size ());
                    socket_.options ().connect_routing_id (routing_id);
                    return 0;
                }
                catch (const std::invalid_argument &) {
                    errno = value_.size () > 255u ? EMSGSIZE : EINVAL;
                    return -1;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int invoke_dealer_set (dealer_option option_, const T &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, dealer_socket_t>::value) {
                try {
                    switch (option_) {
                        case dealer_option::probe:
                            socket_.options ().probe (static_cast<int> (value_) != 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int invoke_pub_set (pub_option option_, const T &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pub_socket_t>::value
                          || std::is_same<socket_type_t, xpub_socket_t>::value) {
                try {
                    switch (option_) {
                        case pub_option::nodrop:
                            socket_.options ().no_drop (static_cast<int> (value_) != 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int invoke_sub_set (sub_option option_, const T &value_)
    {
        (void) option_;
        (void) value_;
        errno = EOPNOTSUPP;
        return -1;
    }

    template <typename T> int invoke_stream_set (stream_option option_, const T &value_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, stream_socket_t>::value) {
                try {
                    switch (option_) {
                        case stream_option::notify:
                            socket_.options ().notify (static_cast<int> (value_) != 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int invoke_router_get (router_option option_, T &value_) const
    {
        return visit_const ([&] (const auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, router_socket_t>::value) {
                try {
                    switch (option_) {
                        case router_option::mandatory:
                            value_ = static_cast<T> (
                              const_cast<socket_type_t &> (socket_).options ().mandatory () ? 1
                                                                                            : 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename T> int invoke_pub_get (pub_option option_, T &value_) const
    {
        return visit_const ([&] (const auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, pub_socket_t>::value
                          || std::is_same<socket_type_t, xpub_socket_t>::value) {
                try {
                    switch (option_) {
                        case pub_option::nodrop:
                            value_ = static_cast<T> (
                              const_cast<socket_type_t &> (socket_).options ().no_drop () ? 1 : 0);
                            return 0;
                        default:
                            errno = EOPNOTSUPP;
                            return -1;
                    }
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    int invoke_routing_id_set (const routing_id_t &routing_id_)
    {
        return visit ([&] (auto &socket_) -> int {
            using socket_type_t = typename std::decay<decltype (socket_)>::type;
            if constexpr (std::is_same<socket_type_t, dealer_socket_t>::value
                          || std::is_same<socket_type_t, router_socket_t>::value
                          || std::is_same<socket_type_t, stream_socket_t>::value) {
                try {
                    socket_.set_routing_id (routing_id_);
                    return 0;
                }
                catch (const binding_error_t &err) {
                    errno = err.internal_errno ();
                    return -1;
                }
            } else {
                errno = EOPNOTSUPP;
                return -1;
            }
        });
    }

    template <typename SocketLike>
    static int
    recv_received_impl (SocketLike &socket_, received_t &received_out_, recv_flags_t flags_)
    {
        return socket_.recv (received_out_, flags_);
    }

    template <typename SocketLike>
    static int recv_single_part_impl (SocketLike &socket_,
                                      routing_id_t *source_rid_out_,
                                      message_t &part_out_,
                                      recv_flags_t flags_)
    {
        using socket_type_t = typename std::decay<SocketLike>::type;
        if constexpr (std::is_same<socket_type_t, pair_socket_t>::value
                      || std::is_same<socket_type_t, dealer_socket_t>::value) {
            if (source_rid_out_ != NULL)
                *source_rid_out_ = routing_id_t::from (std::string ("placeholder"));
            return socket_.recv (part_out_, flags_);
        } else if constexpr (std::is_same<socket_type_t, router_socket_t>::value) {
            if (source_rid_out_ == NULL) {
                routing_id_t ignored = routing_id_t::from (std::string ("placeholder"));
                return socket_.recv (ignored, part_out_, flags_);
            }
            return socket_.recv (*source_rid_out_, part_out_, flags_);
        }

        received_t received;
        const int rc = recv_received_impl (socket_, received, flags_);
        if (rc != 0)
            return rc;
        if (received.parts ().size () != 1) {
            errno = EMSGSIZE;
            return -1;
        }
        if (source_rid_out_ != NULL) {
            if (!received.routing_id ().has_value ()) {
                errno = EPROTO;
                return -1;
            }
            *source_rid_out_ = *received.routing_id ();
        }
        part_out_ = std::move (received.parts ()[0]);
        return 0;
    }

    static int bool_result_to_errno (bool ok_)
    {
        if (ok_)
            return 0;
        errno = EAGAIN;
        return -1;
    }

    socket_variant_t _socket;
    socket_type _type;
};

inline int send_socket (socket_t &socket_, zlink::message_t &part_, int flags_ = 0)
{
    return (socket_.*static_cast<int (socket_t::*) (zlink::message_t &, int)> (&socket_t::send)) (
      part_, flags_);
}

inline int send_socket (socket_t &socket_,
                        const zlink::routing_id_t &routing_id_,
                        zlink::message_t &part_,
                        int flags_ = 0)
{
    return (
      socket_
      .*static_cast<int (socket_t::*) (const zlink::routing_id_t &, zlink::message_t &, int)> (
        &socket_t::send)) (routing_id_, part_, flags_);
}

class socket_guard_t
{
  public:
    socket_guard_t () : _sock () {}

    socket_guard_t (zlink::context_t &ctx_, zlink::socket_type type_) : _sock (ctx_, type_) {}

    socket_t &sock () { return _sock; }
    const socket_t &sock () const { return _sock; }
    bool valid () const { return _sock.valid (); }

  private:
    socket_t _sock;
};

} // namespace perf

#endif
