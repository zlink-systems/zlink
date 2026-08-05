/* SPDX-License-Identifier: MPL-2.0 */
#include <Runtime/Options/socket_options_detail.hpp>

namespace zlink
{

common_socket_options_t::common_socket_options_t () noexcept : _socket (nullptr)
{
}

common_socket_options_t::common_socket_options_t (socket_t &socket_) noexcept : _socket (&socket_)
{
}

router_socket_options_t::router_socket_options_t () noexcept = default;

router_socket_options_t::router_socket_options_t (socket_t &socket_) noexcept :
    common_socket_options_t (socket_)
{
}

dealer_socket_options_t::dealer_socket_options_t () noexcept = default;

dealer_socket_options_t::dealer_socket_options_t (socket_t &socket_) noexcept :
    common_socket_options_t (socket_)
{
}

stream_socket_options_t::stream_socket_options_t () noexcept = default;

stream_socket_options_t::stream_socket_options_t (socket_t &socket_) noexcept :
    common_socket_options_t (socket_)
{
}

pub_socket_options_t::pub_socket_options_t () noexcept = default;

pub_socket_options_t::pub_socket_options_t (socket_t &socket_) noexcept :
    common_socket_options_t (socket_)
{
}

sub_socket_options_t::sub_socket_options_t () noexcept = default;

sub_socket_options_t::sub_socket_options_t (socket_t &socket_) noexcept :
    common_socket_options_t (socket_)
{
}

std::chrono::milliseconds common_socket_options_t::linger () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::linger));
}

void common_socket_options_t::linger (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::linger,
                                          detail::native_option_ms (value));
}

byte_count_t common_socket_options_t::send_hwm () const
{
    return byte_count_t::bytes (detail::get_typed_option_value<uint64_t> (
      detail::native_option_handle (_socket), detail::socket_option_id::sndhwm));
}

void common_socket_options_t::send_hwm (byte_count_t value)
{
    detail::set_typed_option_value<uint64_t> (detail::native_option_handle (_socket),
                                               detail::socket_option_id::sndhwm, value.bytes ());
}

byte_count_t common_socket_options_t::recv_hwm () const
{
    return byte_count_t::bytes (detail::get_typed_option_value<uint64_t> (
      detail::native_option_handle (_socket), detail::socket_option_id::rcvhwm));
}

void common_socket_options_t::recv_hwm (byte_count_t value)
{
    detail::set_typed_option_value<uint64_t> (detail::native_option_handle (_socket),
                                               detail::socket_option_id::rcvhwm, value.bytes ());
}

std::chrono::milliseconds common_socket_options_t::send_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::sndtimeo));
}

void common_socket_options_t::send_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::sndtimeo,
                                          detail::native_option_ms (value));
}

std::chrono::milliseconds common_socket_options_t::recv_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::rcvtimeo));
}

void common_socket_options_t::recv_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::rcvtimeo,
                                          detail::native_option_ms (value));
}

bool common_socket_options_t::immediate () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::socket_option_id::immediate)
           != 0;
}

void common_socket_options_t::immediate (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::immediate, value ? 1 : 0);
}

std::chrono::milliseconds common_socket_options_t::connect_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::connect_timeout));
}

void common_socket_options_t::connect_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::connect_timeout,
                                          detail::native_option_ms (value));
}

bool common_socket_options_t::ipv6 () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::socket_option_id::ipv6)
           != 0;
}

void common_socket_options_t::ipv6 (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::ipv6, value ? 1 : 0);
}

bool common_socket_options_t::tcp_no_delay () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::socket_option_id::tcp_nodelay)
           != 0;
}

void common_socket_options_t::tcp_no_delay (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::tcp_nodelay, value ? 1 : 0);
}

tcp_keepalive_mode_t common_socket_options_t::tcp_keepalive () const
{
    return static_cast<tcp_keepalive_mode_t> (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::tcp_keepalive));
}

void common_socket_options_t::tcp_keepalive (tcp_keepalive_mode_t value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::tcp_keepalive,
                                          static_cast<int> (value));
}

rid_duplicate_policy_t common_socket_options_t::rid_duplicate_policy () const
{
    return static_cast<rid_duplicate_policy_t> (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::rid_duplicate_policy));
}

void common_socket_options_t::rid_duplicate_policy (rid_duplicate_policy_t value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::rid_duplicate_policy,
                                          static_cast<int> (value));
}

byte_size_t common_socket_options_t::max_message_size () const
{
    return byte_size_t::bytes (detail::get_typed_option_value<int64_t> (
      detail::native_option_handle (_socket), detail::socket_option_id::maxmsgsize));
}

void common_socket_options_t::max_message_size (byte_size_t value)
{
    detail::set_typed_option_value<int64_t> (detail::native_option_handle (_socket),
                                              detail::socket_option_id::maxmsgsize, value.bytes ());
}

socket_backlog_t common_socket_options_t::backlog () const
{
    return socket_backlog_t::value (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::backlog));
}

void common_socket_options_t::backlog (socket_backlog_t value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::backlog, value.value ());
}

std::chrono::milliseconds common_socket_options_t::reconnect_interval () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::reconnect_ivl));
}

void common_socket_options_t::reconnect_interval (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::reconnect_ivl,
                                          detail::native_option_ms (value));
}

std::chrono::milliseconds common_socket_options_t::reconnect_interval_max () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::reconnect_ivl_max));
}

void common_socket_options_t::reconnect_interval_max (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::reconnect_ivl_max,
                                          detail::native_option_ms (value));
}

submit_retry_mode_t common_socket_options_t::submit_retry_mode () const
{
    return static_cast<submit_retry_mode_t> (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::submit_retry_mode));
}

void common_socket_options_t::submit_retry_mode (submit_retry_mode_t value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::submit_retry_mode,
                                          static_cast<int> (value));
}

std::chrono::milliseconds common_socket_options_t::submit_retry_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::submit_retry_timeout));
}

void common_socket_options_t::submit_retry_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::submit_retry_timeout,
                                          detail::native_option_ms (value));
}

int common_socket_options_t::submit_retry_attempts () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::socket_option_id::submit_retry_attempts);
}

void common_socket_options_t::submit_retry_attempts (int value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::socket_option_id::submit_retry_attempts, value);
}

std::string common_socket_options_t::last_endpoint () const
{
    return detail::get_typed_option_string (detail::native_option_handle (_socket),
                                             detail::socket_option_id::last_endpoint);
}

} // namespace zlink
