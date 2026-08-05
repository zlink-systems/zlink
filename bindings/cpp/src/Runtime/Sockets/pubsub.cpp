/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

namespace zlink
{

pub_socket_t::pub_socket_t (context_t &ctx_) : publisher_socket_t (ctx_, socket_type::pub)
{
}

send_operation_t pub_socket_t::publish (const std::string &topic_id_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_publish;
    state_ptr->raw.socket = detail::native_handle (*this);
    state_ptr->raw.topic = topic_id_;
    return send_operation_t (std::move (state_ptr));
}

xpub_socket_t::xpub_socket_t (context_t &ctx_) : publisher_socket_t (ctx_, socket_type::xpub)
{
}

send_operation_t xpub_socket_t::publish (const std::string &topic_id_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_publish;
    state_ptr->raw.socket = detail::native_handle (*this);
    state_ptr->raw.topic = topic_id_;
    return send_operation_t (std::move (state_ptr));
}

int xpub_socket_t::receive_subscription_event (subscription_event_t &out_, recv_flags_t flags_)
{
    subscription_event_t event;
    std::vector<char> topic_buffer (256);
    size_t topic_size = topic_buffer.size ();
    const zlink_routing_id_t *source_rid = nullptr;
    int subscribed = 0;
    zlink_recv_result_t rc = static_cast<zlink_recv_result_t> (206);

    while (true) {
        rc = zlink_xpub_recv_part (detail::native_handle (*this), &source_rid, &subscribed,
                                   topic_buffer.data (), topic_buffer.size (), &topic_size,
                                   static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
        if (rc == 0)
            break;
        if (zlink_errno () != EMSGSIZE)
            break;
        topic_buffer.resize (topic_size);
    }

    if (rc == 0) {
        if (source_rid && source_rid->size > 0)
            event.routing_id = zlink::detail::native_routing_id (*source_rid);
        event.subscribed = subscribed != 0;
        event.topic.assign (topic_buffer.data (), topic_size);
    }

    if (rc == 0)
        out_ = std::move (event);
    return static_cast<int> (rc);
}

sub_socket_t::sub_socket_t (context_t &ctx_) : subscriber_socket_t (ctx_, socket_type::sub)
{
}

void sub_socket_t::set_subscription (const std::string &filter_)
{
    if (socket_t::set_subscription (filter_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

void sub_socket_t::unset_subscription (const std::string &filter_)
{
    if (socket_t::unset_subscription (filter_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

void sub_socket_t::subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_)
{
    if (socket_t::subscription_at (index_, filter_out_, is_pattern_out_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

xsub_socket_t::xsub_socket_t (context_t &ctx_) : subscriber_socket_t (ctx_, socket_type::xsub)
{
}

void xsub_socket_t::set_subscription (const std::string &filter_)
{
    if (socket_t::set_subscription (filter_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

void xsub_socket_t::unset_subscription (const std::string &filter_)
{
    if (socket_t::unset_subscription (filter_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

void xsub_socket_t::subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_)
{
    if (socket_t::subscription_at (index_, filter_out_, is_pattern_out_) != 0)
        throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

} // namespace zlink
