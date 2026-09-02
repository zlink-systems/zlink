/* SPDX-License-Identifier: MPL-2.0 */
#include <Runtime/Options/socket_options_detail.hpp>

namespace zlink
{

bool stream_socket_options_t::notify () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::stream_option_id::notify)
           != 0;
}

void stream_socket_options_t::notify (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::stream_option_id::notify, value ? 1 : 0);
}

stream_recv_mode_t stream_socket_options_t::recv_mode () const
{
    return static_cast<stream_recv_mode_t> (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::stream_option_id::recv_mode));
}

void stream_socket_options_t::recv_mode (stream_recv_mode_t mode)
{
    if (mode != stream_recv_mode_t::raw && mode != stream_recv_mode_t::packet)
        throw config_error_t (config_result_t::invalid_argument, EINVAL);
    detail::set_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::stream_option_id::recv_mode,
      static_cast<int> (mode));
}

} // namespace zlink
