/* SPDX-License-Identifier: MPL-2.0 */
#include <Runtime/Options/socket_options_detail.hpp>

namespace zlink
{

bool router_socket_options_t::mandatory () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::router_option_id::mandatory)
           != 0;
}

void router_socket_options_t::mandatory (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::router_option_id::mandatory, value ? 1 : 0);
}

bool router_socket_options_t::handover () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::socket_option_id::rid_duplicate_policy)
           == ZLINK_RID_DUPLICATE_HANDOVER;
}

void router_socket_options_t::handover (bool value)
{
    detail::set_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::socket_option_id::rid_duplicate_policy,
      value ? ZLINK_RID_DUPLICATE_HANDOVER : ZLINK_RID_DUPLICATE_REJECT);
}

bool router_socket_options_t::probe () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                                 detail::router_option_id::probe)
           != 0;
}

void router_socket_options_t::probe (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::router_option_id::probe, value ? 1 : 0);
}

std::optional<routing_id_t> router_socket_options_t::connect_routing_id () const
{
    detail::ensure_config_handle (detail::native_option_handle (_socket));
    zlink_routing_id_t native;
    std::memset (&native, 0, sizeof (native));
    size_t size = sizeof (native);
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (zlink_get_router_option (
      detail::native_option_handle (_socket),
      static_cast<zlink_router_option_t> (detail::router_option_id::connect_routing_id), &native,
      &size)));
    if (native.size == 0)
        return std::nullopt;
    return zlink::detail::native_routing_id (native);
}

void router_socket_options_t::connect_routing_id (const routing_id_t &value)
{
    detail::ensure_config_handle (detail::native_option_handle (_socket));
    const zlink_routing_id_t native = *zlink::detail::routing_id_native (value);
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (zlink_set_router_option (
      detail::native_option_handle (_socket),
      static_cast<zlink_router_option_t> (detail::router_option_id::connect_routing_id),
      native.data, native.size)));
}

std::chrono::milliseconds router_socket_options_t::request_timeout () const
{
    return std::chrono::milliseconds (detail::get_typed_option_value<int> (
      detail::native_option_handle (_socket), detail::router_option_id::request_timeout_ms));
}

void router_socket_options_t::request_timeout (std::chrono::milliseconds value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                          detail::router_option_id::request_timeout_ms,
                                          detail::native_option_ms (value));
}

peer_weight_t router_socket_options_t::peer_weight () const
{
    return peer_weight_t::value (detail::get_typed_option_value<uint32_t> (
      detail::native_option_handle (_socket), detail::router_option_id::weight));
}

void router_socket_options_t::peer_weight (peer_weight_t value)
{
    detail::set_typed_option_value<uint32_t> (detail::native_option_handle (_socket),
                                               detail::router_option_id::weight, value.value ());
}

} // namespace zlink
