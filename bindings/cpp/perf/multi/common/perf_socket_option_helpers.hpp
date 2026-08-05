/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_PERF_MULTI_SOCKET_OPTION_HELPERS_HPP
#define ZLINK_CPP_PERF_MULTI_SOCKET_OPTION_HELPERS_HPP

#include "../../common/perf_socket_adapter.hpp"

#include <cerrno>
#include <chrono>
#include <string>

namespace perf
{
namespace multi
{

template <typename SocketLike, typename T>
inline auto set_common_socket_option_impl (SocketLike &socket,
                                           perf::options::socket_option_key_t<T> key,
                                           const T &value,
                                           int) -> decltype (socket.set_option (key, value), int ())
{
    return socket.set_option (key, value);
}

template <typename SocketLike, typename T>
inline int set_common_socket_option_impl (SocketLike &socket,
                                          perf::options::socket_option_key_t<T> key,
                                          const T &value,
                                          long)
{
    try {
        switch (key.option) {
            case perf::options::socket_option::sndhwm:
                socket.options ().send_hwm (
                  zlink::byte_count_t::bytes (static_cast<uint64_t> (value)));
                return 0;
            case perf::options::socket_option::rcvhwm:
                socket.options ().recv_hwm (
                  zlink::byte_count_t::bytes (static_cast<uint64_t> (value)));
                return 0;
            case perf::options::socket_option::sndtimeo:
                socket.options ().send_timeout (std::chrono::milliseconds (value));
                return 0;
            case perf::options::socket_option::rcvtimeo:
                socket.options ().recv_timeout (std::chrono::milliseconds (value));
                return 0;
            case perf::options::socket_option::linger:
                socket.options ().linger (std::chrono::milliseconds (value));
                return 0;
            default:
                errno = EOPNOTSUPP;
                return -1;
        }
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return -1;
    }
}

template <typename SocketLike, typename T>
inline int set_common_socket_option (SocketLike &socket,
                                     perf::options::socket_option_key_t<T> key,
                                     const T &value)
{
    return set_common_socket_option_impl (socket, key, value, 0);
}

template <typename SocketLike, typename T>
inline auto get_common_socket_option_impl (SocketLike &socket,
                                           perf::options::socket_option_key_t<T> key,
                                           T &value,
                                           int) -> decltype (socket.get_option (key, &value),
                                                             int ())
{
    return socket.get_option (key, &value);
}

template <typename SocketLike, typename T>
inline int get_common_socket_option_impl (SocketLike &socket,
                                          perf::options::socket_option_key_t<T> key,
                                          T &value,
                                          long)
{
    (void) socket;
    (void) key;
    (void) value;
    errno = EOPNOTSUPP;
    return -1;
}

template <typename SocketLike>
inline auto
get_common_socket_option_string_impl (SocketLike &socket,
                                      perf::options::socket_option_key_t<std::string> key,
                                      std::string &value,
                                      int) -> decltype (socket.get_option (key, value), int ())
{
    return socket.get_option (key, value);
}

template <typename SocketLike>
inline int get_common_socket_option_string_impl (
  SocketLike &socket, perf::options::socket_option_key_t<std::string> key, std::string &value, long)
{
    try {
        if (key.option == perf::options::socket_option::last_endpoint) {
            value = socket.options ().last_endpoint ();
            return 0;
        }
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return -1;
    }
    errno = EOPNOTSUPP;
    return -1;
}

template <typename SocketLike, typename T>
inline int
get_common_socket_option (SocketLike &socket, perf::options::socket_option_key_t<T> key, T &value)
{
    return get_common_socket_option_impl (socket, key, value, 0);
}

template <typename SocketLike>
inline int get_common_socket_option (SocketLike &socket,
                                     perf::options::socket_option_key_t<std::string> key,
                                     std::string &value)
{
    return get_common_socket_option_string_impl (socket, key, value, 0);
}

} // namespace multi
} // namespace perf

#endif
