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

} // namespace zlink
