/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_SUBSCRIPTION_READER_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_SUBSCRIPTION_READER_HPP_INCLUDED

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Native/native_message_parts.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

struct topic_message_access_t
{
    static void replace_single (topic_message_t &message_out_,
                                 std::optional<routing_id_t> source_,
                                 std::string topic_,
                                 message_t part_)
    {
        message_out_._routing_id = std::move (source_);
        message_out_._topic = std::move (topic_);
        message_out_._parts.replace (std::move (part_));
    }

    static void replace_parts (topic_message_t &message_out_,
                               std::optional<routing_id_t> source_,
                               std::string topic_,
                               std::vector<message_t> &parts_)
    {
        message_out_._routing_id = std::move (source_);
        message_out_._topic = std::move (topic_);
        message_out_._parts.replace (parts_);
    }
};

inline std::optional<routing_id_t>
optional_native_routing_id (const zlink_routing_id_t *routing_id_)
{
    return routing_id_ && routing_id_->size > 0
             ? std::optional<routing_id_t> (native_routing_id (*routing_id_))
             : std::nullopt;
}

inline routing_id_t routing_id_or_empty (const zlink_routing_id_t *routing_id_)
{
    return routing_id_ && routing_id_->size > 0 ? native_routing_id (*routing_id_)
                                                : unchecked_empty_routing_id ();
}

template <typename ReceiveMessage>
[[nodiscard]] int read_subscription_message (topic_message_t &message_out_,
                                             ReceiveMessage &&receive_message_)
{
    std::vector<char> topic_buffer (256u);
    std::vector<zlink_msg_t> native_parts (native_part_stack_capacity);
    const zlink_routing_id_t *source_rid = nullptr;
    size_t topic_length = 0;
    size_t part_count = 0;
    int rc = ZLINK_RECV_INTERNAL_ERROR;

    for (;;) {
        rc = receive_message_ (
          &source_rid, topic_buffer.data (), topic_buffer.size (), &topic_length,
          native_parts.data (), native_parts.size (), &part_count);
        if (rc != ZLINK_RECV_BUFFER_TOO_SMALL)
            break;
        if (topic_length <= topic_buffer.size () && part_count <= native_parts.size ()) {
            errno = EPROTO;
            return -1;
        }
        if (topic_length > topic_buffer.size ())
            topic_buffer.resize (topic_length);
        if (part_count > native_parts.size ())
            native_parts.resize (part_count);
    }

    if (rc != ZLINK_RECV_OK)
        return rc;
    if (part_count == 0 || part_count > native_parts.size ()
        || topic_length > topic_buffer.size ()) {
        if (part_count <= native_parts.size ())
            close_message_array (native_parts.data (), part_count);
        errno = EPROTO;
        return -1;
    }

    const std::optional<routing_id_t> source = optional_native_routing_id (source_rid);
    std::string topic (topic_buffer.data (), topic_length);
    std::vector<message_t> parts = take_parts_from_native (native_parts.data (), part_count);
    if (parts.size () == 1u) {
        message_t part = std::move (parts[0]);
        topic_message_access_t::replace_single (
          message_out_, source, std::move (topic), std::move (part));
    } else {
        topic_message_access_t::replace_parts (
          message_out_, source, std::move (topic), parts);
    }
    return 0;
}

} // namespace detail
} // namespace zlink

#endif
