/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"
#include "lazy_message_parts.hpp"
#include "message.hpp"

#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zlink
{

namespace detail
{
struct topic_message_access_t;
}

/// @brief A received publish: its topic and message parts.
class topic_message_t
{
  public:
    topic_message_t () = default;
    ~topic_message_t ();
    topic_message_t (const topic_message_t &) = default;
    topic_message_t &operator= (const topic_message_t &) = default;
    topic_message_t (topic_message_t &&) noexcept = default;
    topic_message_t &operator= (topic_message_t &&) noexcept = default;

    topic_message_t (std::optional<routing_id_t> routing_id_,
                     std::string topic_,
                     std::vector<message_t> parts_) :
        _routing_id (std::move (routing_id_)),
        _topic (std::move (topic_)),
        _parts (std::move (parts_))
    {
    }

    const std::optional<routing_id_t> &routing_id () const noexcept { return _routing_id; }

    const std::string &topic () const noexcept { return _topic; }
    const std::vector<message_t> &parts () const;
    std::vector<message_t> &parts ();

    bool is_single_part () const noexcept
    {
        return _parts.is_single_part ();
    }
    message_t &first_part ();
    message_t single_part_or_throw ();
    void close ();

  private:
    topic_message_t (std::optional<routing_id_t> routing_id_, std::string topic_, message_t part_) :
        _routing_id (std::move (routing_id_)),
        _topic (std::move (topic_)),
        _parts (std::move (part_))
    {
    }

    std::optional<routing_id_t> _routing_id;
    std::string _topic;
    detail::lazy_message_parts_t _parts;
    friend struct detail::topic_message_access_t;
};

} // namespace zlink
