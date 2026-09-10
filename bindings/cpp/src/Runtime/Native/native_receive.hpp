/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED

#include "native_message_parts.hpp"
#include "../Core/routing_id_access.hpp"

#include <zlink/Contracts/Messaging/lazy_message_parts.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <cerrno>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

struct recv_envelope_t
{
    routing_id_t source_rid;
    bool has_reply_token;
    zlink_reply_token_t reply_token;
    lazy_message_parts_t *parts;
    std::vector<zlink_msg_t> native_parts;
    size_t native_part_count;
    std::optional<routing_id_t> subscription_source;
    std::string subscription_topic;
    std::vector<message_t> subscription_parts;
    size_t subscription_index;

    void bind (lazy_message_parts_t &parts_) noexcept { parts = &parts_; }

    void reset () noexcept
    {
        release_native_parts ();
        clear_subscription ();
        source_rid = zlink::detail::unchecked_empty_routing_id ();
        has_reply_token = false;
        reply_token = 0;
        if (parts)
            parts->prepare_receive ();
    }

    void receive_single_part (message_t part_) { parts->receive_single_part (std::move (part_)); }

    void reserve_parts (size_t part_count_) { parts->reserve_receive_parts (part_count_); }

    void receive_part (message_t part_) { parts->receive_part (std::move (part_)); }

    void prepare_native_buffer ()
    {
        if (native_parts.empty ())
            native_parts.resize (native_part_stack_capacity);
    }

    void grow_native_buffer (size_t part_count_) { native_parts.resize (part_count_); }

    zlink_msg_t *native_buffer () noexcept { return native_parts.data (); }

    size_t native_capacity () const noexcept { return native_parts.size (); }

    void own_native_parts (size_t part_count_) noexcept { native_part_count = part_count_; }

    int receive_native_parts ()
    {
        if (native_part_count == 1u) {
            message_t part;
            if (!part.valid () || !adopt_native_part (part, native_parts[0]))
                return -1;
            receive_single_part (std::move (part));
        } else {
            reserve_parts (native_part_count);
            for (size_t i = 0; i < native_part_count; ++i) {
                message_t part;
                if (!part.valid () || !adopt_native_part (part, native_parts[i]))
                    return -1;
                receive_part (std::move (part));
            }
        }

        release_native_parts ();
        return 0;
    }

    void release_native_parts () noexcept
    {
        if (native_part_count != 0)
            close_message_array (native_parts.data (), native_part_count);
        native_part_count = 0;
    }

    void clear_subscription () noexcept
    {
        subscription_source.reset ();
        subscription_topic.clear ();
        subscription_parts.clear ();
        subscription_index = 0;
    }

    bool has_subscription_part () const noexcept
    {
        return subscription_index < subscription_parts.size ();
    }

    void stage_subscription (topic_message_t &message_)
    {
        clear_subscription ();
        subscription_source = message_.routing_id ();
        subscription_topic = message_.topic ();
        subscription_parts = std::move (message_.parts ());
    }

    void take_subscription_part (std::optional<routing_id_t> &source_out_,
                                 std::string &topic_out_,
                                 message_t &part_out_,
                                 bool &has_more_out_)
    {
        source_out_ = subscription_source;
        topic_out_ = subscription_topic;
        part_out_ = std::move (subscription_parts[subscription_index++]);
        has_more_out_ = has_subscription_part ();
        if (!has_more_out_)
            clear_subscription ();
    }

    void take_subscription_message (topic_message_t &message_out_)
    {
        std::vector<message_t> remaining;
        remaining.reserve (subscription_parts.size () - subscription_index);
        while (subscription_index < subscription_parts.size ())
            remaining.push_back (std::move (subscription_parts[subscription_index++]));
        message_out_ = topic_message_t (
          subscription_source, subscription_topic, std::move (remaining));
        clear_subscription ();
    }

    recv_envelope_t () :
        source_rid (zlink::detail::unchecked_empty_routing_id ()),
        has_reply_token (false),
        reply_token (0),
        parts (nullptr),
        native_part_count (0),
        subscription_index (0)
    {
    }

    ~recv_envelope_t () { release_native_parts (); }
};

inline int recv_router (void *socket_, const zlink_routing_id_t **source_rid_,
                        zlink_reply_token_t *reply_token_, zlink_msg_t *parts_,
                        size_t parts_capacity_, size_t *part_count_, recv_flags_t flags_)
{
    return zlink_router_recv (
      socket_, source_rid_, reply_token_, parts_, parts_capacity_, part_count_,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
}

inline int recv_basic (void *socket_, const zlink_routing_id_t **source_rid_,
                       zlink_msg_t *parts_, size_t parts_capacity_, size_t *part_count_,
                       recv_flags_t flags_)
{
    return zlink_recv (
      socket_, source_rid_, parts_, parts_capacity_, part_count_,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
}

inline int recv_whole_envelope (void *socket_,
                                recv_flags_t flags_,
                                recv_envelope_t &envelope_,
                                bool use_router_recv_)
{
    envelope_.reset ();
    envelope_.prepare_native_buffer ();

    const zlink_routing_id_t *source_rid = nullptr;
    zlink_reply_token_t reply_token = 0;
    size_t part_count = 0;
    int rc = ZLINK_RECV_INTERNAL_ERROR;

    for (;;) {
        rc = use_router_recv_
               ? recv_router (socket_, &source_rid, &reply_token,
                              envelope_.native_buffer (), envelope_.native_capacity (),
                              &part_count, flags_)
               : recv_basic (socket_, &source_rid, envelope_.native_buffer (),
                             envelope_.native_capacity (), &part_count, flags_);
        if (rc != ZLINK_RECV_BUFFER_TOO_SMALL)
            break;
        if (part_count <= envelope_.native_capacity ()) {
            errno = EPROTO;
            return -1;
        }
        envelope_.grow_native_buffer (part_count);
    }

    if (rc != ZLINK_RECV_OK)
        return use_router_recv_ ? -1 : rc;
    if (part_count == 0 || part_count > envelope_.native_capacity ()) {
        errno = EPROTO;
        return -1;
    }

    envelope_.own_native_parts (part_count);
    if (source_rid && source_rid->size > 0)
        envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
    if (reply_token != 0) {
        envelope_.has_reply_token = true;
        envelope_.reply_token = reply_token;
    }

    return envelope_.receive_native_parts ();
}

} // namespace detail
} // namespace zlink

#endif
