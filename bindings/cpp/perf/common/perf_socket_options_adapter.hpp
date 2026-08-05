/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_PERF_SOCKET_OPTIONS_ADAPTER_HPP
#define ZLINK_CPP_PERF_SOCKET_OPTIONS_ADAPTER_HPP

#include <cstdint>
#include <string>

namespace perf
{
namespace options
{

enum class socket_option
{
    linger,
    sndhwm,
    rcvhwm,
    sndtimeo,
    rcvtimeo,
    last_endpoint,
    tcp_nodelay,
    connect_timeout,
    type
};

enum class router_option
{
    mandatory,
    probe,
    connect_routing_id
};

enum class dealer_option
{
    probe
};

enum class pub_option
{
    nodrop
};

enum class sub_option
{
    topics_count
};

enum class stream_option
{
    notify
};

template <typename T> struct socket_option_key_t
{
    explicit constexpr socket_option_key_t (socket_option option_) : option (option_) {}

    socket_option option;
};

template <typename T> struct router_option_key_t
{
    explicit constexpr router_option_key_t (router_option option_) : option (option_) {}

    router_option option;
};

template <typename T> struct dealer_option_key_t
{
    explicit constexpr dealer_option_key_t (dealer_option option_) : option (option_) {}

    dealer_option option;
};

template <typename T> struct pub_option_key_t
{
    explicit constexpr pub_option_key_t (pub_option option_) : option (option_) {}

    pub_option option;
};

template <typename T> struct sub_option_key_t
{
    explicit constexpr sub_option_key_t (sub_option option_) : option (option_) {}

    sub_option option;
};

template <typename T> struct stream_option_key_t
{
    explicit constexpr stream_option_key_t (stream_option option_) : option (option_) {}

    stream_option option;
};

namespace socket_options
{
static const socket_option_key_t<int> linger (socket_option::linger);
static const socket_option_key_t<uint64_t> sndhwm (socket_option::sndhwm);
static const socket_option_key_t<uint64_t> rcvhwm (socket_option::rcvhwm);
static const socket_option_key_t<int> sndtimeo (socket_option::sndtimeo);
static const socket_option_key_t<int> rcvtimeo (socket_option::rcvtimeo);
static const socket_option_key_t<std::string> last_endpoint (socket_option::last_endpoint);
static const socket_option_key_t<int> tcp_nodelay (socket_option::tcp_nodelay);
} // namespace socket_options

namespace router_options
{
static const router_option_key_t<int> mandatory (router_option::mandatory);
} // namespace router_options

} // namespace options

using options::dealer_option;
using options::dealer_option_key_t;
using options::pub_option;
using options::pub_option_key_t;
using options::router_option;
using options::router_option_key_t;
using options::socket_option;
using options::socket_option_key_t;
using options::stream_option;
using options::stream_option_key_t;
using options::sub_option;
using options::sub_option_key_t;

} // namespace perf

#endif
