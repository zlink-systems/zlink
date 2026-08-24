/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_SUBSCRIPTION_READER_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_SUBSCRIPTION_READER_HPP_INCLUDED

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Native/native_message_guard.hpp>

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

inline size_t bounded_topic_size (size_t length_, size_t capacity_) noexcept
{
    return length_ < capacity_ ? length_ : capacity_ - 1u;
}

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

inline void assign_subscription_metadata (std::optional<routing_id_t> *source_out_,
                                          std::string &topic_out_,
                                          bool &has_more_out_,
                                          const zlink_routing_id_t *source_,
                                          const char *topic_,
                                          size_t topic_length_,
                                          size_t topic_capacity_,
                                          zlink_part_flag_t has_more_)
{
    if (source_out_)
        *source_out_ = optional_native_routing_id (source_);
    topic_out_.assign (topic_, bounded_topic_size (topic_length_, topic_capacity_));
    has_more_out_ = has_more_ != ZLINK_PART_FINAL;
}

inline void assign_subscription_part (std::optional<routing_id_t> *source_out_,
                                      std::string &topic_out_,
                                      message_t &part_out_,
                                      bool &has_more_out_,
                                      const zlink_routing_id_t *source_,
                                      const char *topic_,
                                      size_t topic_length_,
                                      size_t topic_capacity_,
                                      zlink_msg_t *part_,
                                      zlink_part_flag_t has_more_)
{
    assign_subscription_metadata (source_out_, topic_out_, has_more_out_, source_, topic_,
                                  topic_length_, topic_capacity_, has_more_);
    adopt_native_message (part_out_, part_);
}

template <typename ReceivePart>
[[nodiscard]] int read_subscription_message (topic_message_t &message_out_,
                                             ReceivePart &&receive_part_)
{
    char topic_buffer[256];
    const zlink_routing_id_t *source_rid = nullptr;
    size_t topic_length = sizeof (topic_buffer);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    scoped_native_message_t native_part;
    if (!native_part.init ())
        return -1;

    const int first_rc = receive_part_ (&source_rid, topic_buffer, sizeof (topic_buffer),
                                        &topic_length, native_part.get (), &has_more);
    if (first_rc != ZLINK_RECV_OK) {
        return first_rc;
    }

    std::string topic (topic_buffer, bounded_topic_size (topic_length, sizeof (topic_buffer)));
    const std::optional<routing_id_t> source = optional_native_routing_id (source_rid);

    message_t first_part;
    adopt_native_message (first_part, native_part.get ());

    if (has_more == ZLINK_PART_FINAL) {
        topic_message_access_t::replace_single (message_out_, source, std::move (topic),
                                                 std::move (first_part));
        return 0;
    }

    std::vector<message_t> parts;
    parts.reserve (2);
    parts.push_back (std::move (first_part));

    for (;;) {
        scoped_native_message_t next_part;
        if (!next_part.init ())
            return -1;

        size_t next_topic_length = sizeof (topic_buffer);
        const int rc = receive_part_ (
          &source_rid, topic_buffer, sizeof (topic_buffer),
          &next_topic_length, next_part.get (), &has_more);
        if (rc != ZLINK_RECV_OK) {
            return rc;
        }

        parts.emplace_back ();
        adopt_native_message (parts.back (), next_part.get ());

        if (has_more == ZLINK_PART_FINAL) {
            topic_message_access_t::replace_parts (
              message_out_, source, std::move (topic), parts);
            return 0;
        }
    }
}

} // namespace detail
} // namespace zlink

#endif
