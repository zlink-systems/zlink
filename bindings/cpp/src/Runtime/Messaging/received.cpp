/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Errors/errors.hpp>

#include <Runtime/Messaging/operation_state.hpp>
#include <Runtime/Messaging/received_access.hpp>

namespace zlink
{
namespace
{

inline recv_error_t invalid_single_part_error ()
{
    return recv_error_t (recv_result_t::not_supported, EMSGSIZE);
}

void close_parts (std::vector<message_t> &parts_)
{
    for (std::vector<message_t>::iterator it = parts_.begin (); it != parts_.end (); ++it)
        it->close ();
    parts_.clear ();
}

} // namespace

received_t::~received_t ()
{
    close ();
}

topic_message_t::~topic_message_t ()
{
    close ();
}

received_t::received_t (std::optional<routing_id_t> routing_id_,
                        std::optional<reply_token_t> reply_token_,
                        std::vector<message_t> parts_) :
    _routing_id (std::move (routing_id_)),
    _reply_token (std::move (reply_token_)),
    _parts (std::move (parts_))
{
}

received_t::received_t (std::optional<routing_id_t> routing_id_,
                        std::optional<reply_token_t> reply_token_,
                        message_t part_) :
    _routing_id (std::move (routing_id_)),
    _reply_token (std::move (reply_token_)),
    _parts (std::move (part_))
{
}

message_t &detail::lazy_message_parts_t::first_part ()
{
    if (!is_single_part ())
        throw invalid_single_part_error ();
    if (_single_part.has_value ())
        return *_single_part;
    return _parts.front ();
}

void detail::lazy_message_parts_t::materialize_parts () const
{
    if (!_single_part.has_value ())
        return;
    message_t part = std::move (*_single_part);
    _single_part.reset ();
    _parts.push_back (std::move (part));
}

const std::vector<message_t> &detail::lazy_message_parts_t::parts () const
{
    materialize_parts ();
    return _parts;
}

std::vector<message_t> &detail::lazy_message_parts_t::parts ()
{
    materialize_parts ();
    return _parts;
}

message_t detail::lazy_message_parts_t::single_part_or_throw ()
{
    if (!is_single_part ())
        throw invalid_single_part_error ();
    if (_single_part.has_value ())
        return *_single_part;
    return _parts.front ();
}

void detail::lazy_message_parts_t::replace (std::vector<message_t> &parts_)
{
    if (_single_part.has_value ())
        _single_part->close ();
    _single_part.reset ();
    close_parts (_parts);
    _parts.reserve (parts_.size ());
    for (auto &part : parts_)
        _parts.push_back (std::move (part));
    parts_.clear ();
}

void detail::lazy_message_parts_t::replace (message_t part_)
{
    if (_single_part.has_value ())
        _single_part->close ();
    _single_part = std::move (part_);
    close_parts (_parts);
}

void detail::lazy_message_parts_t::close ()
{
    if (_single_part.has_value ())
        _single_part->close ();
    _single_part.reset ();
    close_parts (_parts);
}

message_t &received_t::first_part ()
{
    return _parts.first_part ();
}

const std::vector<message_t> &topic_message_t::parts () const
{
    return _parts.parts ();
}

std::vector<message_t> &topic_message_t::parts ()
{
    return _parts.parts ();
}

message_t received_t::single_part_or_throw ()
{
    return _parts.single_part_or_throw ();
}

void received_t::close ()
{
    _parts.close ();
}

void topic_message_t::close ()
{
    _parts.close ();
}

const std::vector<message_t> &received_t::parts () const
{
    return _parts.parts ();
}

std::vector<message_t> &received_t::parts ()
{
    return _parts.parts ();
}

message_t topic_message_t::single_part_or_throw ()
{
    return _parts.single_part_or_throw ();
}

message_t &topic_message_t::first_part ()
{
    return _parts.first_part ();
}

// Send/reply builders retain the routing context encapsulated by received_t.
send_operation_t received_t::send ()
{
    if (!detail::received_access_t::has_send_context (*this))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_routed_send;
    state_ptr->raw.socket = detail::received_access_t::send_handle (*this);
    detail::bind_runtime_state (state_ptr->raw,
                                detail::received_access_t::runtime (*this));
    detail::cache_first_rid_native (state_ptr->raw.target, *_routing_id);
    return send_operation_t (std::move (state_ptr));
}

reply_operation_t received_t::reply ()
{
    if (!detail::received_access_t::has_reply_context (*this))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    auto state_ptr = detail::acquire_state ();
    state_ptr->kind = detail::operation_kind_t::raw_reply;
    state_ptr->raw.socket = detail::received_access_t::send_handle (*this);
    detail::bind_runtime_state (state_ptr->raw,
                                detail::received_access_t::runtime (*this));
    state_ptr->raw.target.first_rid = *_routing_id;
    state_ptr->reply.token = *_reply_token;
    return reply_operation_t (std::move (state_ptr));
}

} // namespace zlink
