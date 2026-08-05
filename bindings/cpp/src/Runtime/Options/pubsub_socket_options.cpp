/* SPDX-License-Identifier: MPL-2.0 */
#include <Runtime/Options/socket_options_detail.hpp>

namespace zlink
{

bool pub_socket_options_t::verbose () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::verbose)
           != 0;
}

void pub_socket_options_t::verbose (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                       detail::pub_option_id::verbose, value ? 1 : 0);
}

bool pub_socket_options_t::verboser () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::verboser)
           != 0;
}

void pub_socket_options_t::verboser (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                       detail::pub_option_id::verboser, value ? 1 : 0);
}

bool pub_socket_options_t::no_drop () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::nodrop)
           != 0;
}

void pub_socket_options_t::no_drop (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                       detail::pub_option_id::nodrop, value ? 1 : 0);
}

bool pub_socket_options_t::manual () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::manual)
           != 0;
}

void pub_socket_options_t::manual (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                       detail::pub_option_id::manual, value ? 1 : 0);
}

bool pub_socket_options_t::manual_last_value () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::manual_last_value)
           != 0;
}

void pub_socket_options_t::manual_last_value (bool value)
{
    detail::set_typed_option_value<int> (detail::native_option_handle (_socket),
                                       detail::pub_option_id::manual_last_value, value ? 1 : 0);
}

message_t pub_socket_options_t::welcome_message () const
{
    const std::string value = detail::get_typed_option_string (detail::native_option_handle (_socket),
                                                             detail::pub_option_id::welcome_msg);
    return message_t::from (std::as_bytes (std::span<const char> (value.data (), value.size ())));
}

void pub_socket_options_t::welcome_message (const message_t &message)
{
    detail::ensure_config_handle (detail::native_option_handle (_socket));
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_set_pub_option (detail::native_option_handle (_socket), ZLINK_PUB_OPT_WELCOME_MSG,
                            message.data (), message.size ())));
}

void pub_socket_options_t::approve_subscribe (const routing_id_t &routing_id)
{
    detail::ensure_config_handle (detail::native_option_handle (_socket));
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_set_pub_option (detail::native_option_handle (_socket), ZLINK_PUB_OPT_APPROVE_SUBSCRIBE,
                            routing_id.data (), routing_id.size ())));
}

void pub_socket_options_t::reject_subscribe (const routing_id_t &routing_id)
{
    detail::ensure_config_handle (detail::native_option_handle (_socket));
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_set_pub_option (detail::native_option_handle (_socket), ZLINK_PUB_OPT_REJECT_SUBSCRIBE,
                            routing_id.data (), routing_id.size ())));
}

int pub_socket_options_t::topics_count () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::pub_option_id::topics_count);
}

int sub_socket_options_t::topics_count () const
{
    return detail::get_typed_option_value<int> (detail::native_option_handle (_socket),
                                              detail::sub_option_id::topics_count);
}

} // namespace zlink
