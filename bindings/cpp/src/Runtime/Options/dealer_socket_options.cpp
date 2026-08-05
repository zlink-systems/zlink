/* SPDX-License-Identifier: MPL-2.0 */
#include <Runtime/Options/socket_options_detail.hpp>

namespace zlink
{

bool dealer_socket_options_t::probe () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::dealer_option_id::probe)
           != 0;
}

void dealer_socket_options_t::probe (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::dealer_option_id::probe, value ? 1 : 0);
}

std::chrono::milliseconds dealer_socket_options_t::request_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::dealer_option_id::request_timeout_ms));
}

void dealer_socket_options_t::request_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::dealer_option_id::request_timeout_ms,
                                          detail::native_option_ms (value));
}

peer_weight_t dealer_socket_options_t::peer_weight () const
{
    return peer_weight_t::value (detail::get_typed_option_value<uint32_t> (
      detail::native_option_handle (_socket), detail::dealer_option_id::weight));
}

void dealer_socket_options_t::peer_weight (peer_weight_t value)
{
    detail::set_typed_option_value<uint32_t> (detail::native_option_handle (_socket),
                                               detail::dealer_option_id::weight, value.value ());
}

} // namespace zlink
