/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Sockets/results.hpp"
#include "message.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

class lazy_message_parts_t
{
  public:
    lazy_message_parts_t () = default;
    explicit lazy_message_parts_t (std::vector<message_t> parts_) : _parts (std::move (parts_)) {}
    explicit lazy_message_parts_t (message_t part_) : _single_part (std::move (part_)) {}

    const std::vector<message_t> &parts () const;
    std::vector<message_t> &parts ();

    bool is_single_part () const noexcept
    {
        return _single_part.has_value () || _parts.size () == 1u;
    }

    message_t &first_part ();
    message_t single_part_or_throw ();
    void close ();

  private:
    void materialize_parts () const;

    mutable std::optional<message_t> _single_part;
    mutable std::vector<message_t> _parts;
};

} // namespace detail
} // namespace zlink
