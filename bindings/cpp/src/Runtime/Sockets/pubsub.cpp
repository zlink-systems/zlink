/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <Runtime/Sockets/detail.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <memory>

namespace zlink
{

namespace
{

// Single owner of the "describe a publish operation" step. PUB and XPUB differ
// only in socket type, not in how a publish operation is set up, so the
// sequence lives here once instead of being repeated per socket class.
//
// The callback state is supplied as a callable, not as an already-evaluated
// reference: callback_state() allocates lazily, so the observable step order of
// the pre-dedup code (topic validation first, then socket handle, then callback
// state, then topic assignment) must be preserved exactly.
std::unique_ptr<detail::operation_state_t>
make_publish_state (socket_t &socket_,
                    const std::string &topic_id_)
{
    detail::validate_no_embedded_null (topic_id_, "topic");
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_publish;
    state_ptr->raw.socket = detail::native_handle (socket_);
    detail::bind_runtime_state (state_ptr->raw, detail::runtime_state (socket_));
    state_ptr->raw.topic = topic_id_;
    return state_ptr;
}

// Single owner of the "configuration call failed" translation used by every
// subscription accessor below.
void throw_last_config_error ()
{
    throw config_error_t (detail::config_result_from_errno (zlink_errno ()), zlink_errno ());
}

} // namespace

pub_socket_t::pub_socket_t (context_t &ctx_) : publisher_socket_t (ctx_, socket_type::pub)
{
}

publish_operation_t pub_socket_t::publish (const std::string &topic_id_)
{
    return publish_operation_t (make_publish_state (*this, topic_id_));
}

xpub_socket_t::xpub_socket_t (context_t &ctx_) : publisher_socket_t (ctx_, socket_type::xpub)
{
}

publish_operation_t xpub_socket_t::publish (const std::string &topic_id_)
{
    return publish_operation_t (make_publish_state (*this, topic_id_));
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
        throw_last_config_error ();
}

void sub_socket_t::unset_subscription (const std::string &filter_)
{
    if (socket_t::unset_subscription (filter_) != 0)
        throw_last_config_error ();
}

void sub_socket_t::subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_)
{
    if (socket_t::subscription_at (index_, filter_out_, is_pattern_out_) != 0)
        throw_last_config_error ();
}

xsub_socket_t::xsub_socket_t (context_t &ctx_) : subscriber_socket_t (ctx_, socket_type::xsub)
{
}

void xsub_socket_t::set_subscription (const std::string &filter_)
{
    if (socket_t::set_subscription (filter_) != 0)
        throw_last_config_error ();
}

void xsub_socket_t::unset_subscription (const std::string &filter_)
{
    if (socket_t::unset_subscription (filter_) != 0)
        throw_last_config_error ();
}

void xsub_socket_t::subscription_at (size_t index_, std::string &filter_out_, bool *is_pattern_out_)
{
    if (socket_t::subscription_at (index_, filter_out_, is_pattern_out_) != 0)
        throw_last_config_error ();
}

} // namespace zlink
