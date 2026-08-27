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

struct recv_envelope_t;

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
    // Replaces a receive result while retaining the vector capacity owned by
    // this envelope for the next caller-provided receive.
    void replace (std::vector<message_t> &parts_);
    void replace (message_t part_);
    void close ();

  private:
    friend struct recv_envelope_t;

    void materialize_parts () const;
    void prepare_receive () noexcept { close (); }
    void receive_single_part (message_t part_) { _single_part.emplace (std::move (part_)); }
    void reserve_receive_parts (size_t part_count_) { _parts.reserve (part_count_); }
    void receive_part (message_t part_) { _parts.emplace_back (std::move (part_)); }

    mutable std::optional<message_t> _single_part;
    mutable std::vector<message_t> _parts;
};

} // namespace detail
} // namespace zlink
