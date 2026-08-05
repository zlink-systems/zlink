/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_OPTIONS_SOCKET_OPTIONS_DETAIL_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_OPTIONS_SOCKET_OPTIONS_DETAIL_HPP_INCLUDED

#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Sockets/socket_options.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Native/native_options.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Options/option_ids.hpp>

#include <zlink.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>

namespace zlink
{
namespace detail
{

inline void ensure_config_handle (void *handle_)
{
    if (!handle_)
        throw config_error_t (config_result_t::invalid_handle, EINVAL);
}

inline void *native_option_handle (socket_t *socket_)
{
    if (!socket_)
        throw config_error_t (config_result_t::invalid_handle, EINVAL);
    return native_handle (*socket_);
}

template <typename T, typename NativeOption, typename Getter>
inline T get_option_value (void *handle_, NativeOption option_, Getter getter_)
{
    ensure_config_handle (handle_);
    T value{};
    size_t size = sizeof (value);
    detail::throw_if_failed<config_error_t> (
      static_cast<config_result_t> (getter_ (handle_, option_, &value, &size)));
    return value;
}

template <typename T, typename NativeOption, typename Setter>
inline void set_option_value (void *handle_, NativeOption option_, const T &value_, Setter setter_)
{
    ensure_config_handle (handle_);
    detail::throw_if_failed<config_error_t> (
      static_cast<config_result_t> (setter_ (handle_, option_, &value_, sizeof (value_))));
}

template <typename NativeOption, typename Getter>
std::string
get_option_string_value (void *handle_, NativeOption option_, size_t initial_cap_, Getter getter_)
{
    ensure_config_handle (handle_);
    config_result_t result = config_result_t::ok;
    std::string value;
    const int rc = detail::read_growing_string (
      [&] (char *buffer_, size_t, size_t *size_out_) {
          result = static_cast<config_result_t> (getter_ (handle_, option_, buffer_, size_out_));
          return result == config_result_t::ok ? 0 : -1;
      },
      initial_cap_, value);
    if (rc == 0)
        return value;
    throw config_error_t (result, zlink_errno ());
}

template <typename OptionId> struct option_traits;

template <> struct option_traits<socket_option_id>
{
    using native_option_t = zlink_option_t;

    static native_option_t native (socket_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_option (handle_, option_, value_, size_);
    }

    static zlink_config_result_t set (void *handle_, native_option_t option_, const void *value_,
                                      size_t size_)
    {
        return zlink_set_option (handle_, option_, value_, size_);
    }

    static size_t string_cap (socket_option_id option_)
    {
        return option_ == socket_option_id::last_endpoint ? 1024u : 256u;
    }
};

template <> struct option_traits<router_option_id>
{
    using native_option_t = zlink_router_option_t;

    static native_option_t native (router_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_router_option (handle_, option_, value_, size_);
    }

    static zlink_config_result_t set (void *handle_, native_option_t option_, const void *value_,
                                      size_t size_)
    {
        return zlink_set_router_option (handle_, option_, value_, size_);
    }
};

template <> struct option_traits<dealer_option_id>
{
    using native_option_t = zlink_dealer_option_t;

    static native_option_t native (dealer_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_dealer_option (handle_, option_, value_, size_);
    }

    static zlink_config_result_t set (void *handle_, native_option_t option_, const void *value_,
                                      size_t size_)
    {
        return zlink_set_dealer_option (handle_, option_, value_, size_);
    }
};

template <> struct option_traits<pub_option_id>
{
    using native_option_t = zlink_pub_option_t;

    static native_option_t native (pub_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_pub_option (handle_, option_, value_, size_);
    }

    static zlink_config_result_t set (void *handle_, native_option_t option_, const void *value_,
                                      size_t size_)
    {
        return zlink_set_pub_option (handle_, option_, value_, size_);
    }

    static size_t string_cap (pub_option_id)
    {
        return 256u;
    }
};

template <> struct option_traits<sub_option_id>
{
    using native_option_t = zlink_sub_option_t;

    static native_option_t native (sub_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_sub_option (handle_, option_, value_, size_);
    }
};

template <> struct option_traits<stream_option_id>
{
    using native_option_t = zlink_stream_option_t;

    static native_option_t native (stream_option_id option_)
    {
        return static_cast<native_option_t> (option_);
    }

    static zlink_config_result_t get (void *handle_, native_option_t option_, void *value_,
                                      size_t *size_)
    {
        return zlink_get_stream_option (handle_, option_, value_, size_);
    }

    static zlink_config_result_t set (void *handle_, native_option_t option_, const void *value_,
                                      size_t size_)
    {
        return zlink_set_stream_option (handle_, option_, value_, size_);
    }
};

template <typename T, typename OptionId>
T get_typed_option_value (void *handle_, OptionId option_)
{
    using traits = option_traits<OptionId>;
    return get_option_value<T> (handle_, traits::native (option_), traits::get);
}

template <typename T, typename OptionId>
void set_typed_option_value (void *handle_, OptionId option_, const T &value_)
{
    using traits = option_traits<OptionId>;
    set_option_value<T> (handle_, traits::native (option_), value_, traits::set);
}

template <typename OptionId>
std::string get_typed_option_string (void *handle_, OptionId option_)
{
    using traits = option_traits<OptionId>;
    return get_option_string_value (handle_, traits::native (option_), traits::string_cap (option_),
                                    traits::get);
}

} // namespace detail
} // namespace zlink

#endif
