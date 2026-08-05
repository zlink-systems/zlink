/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"
#include "../Sockets/results.hpp"
#include "lazy_message_parts.hpp"
#include "message.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace zlink
{

namespace detail
{
struct received_access_t;
}

/// @brief A received message envelope: routing metadata, parts, and an optional reply context.
class received_t
{
  public:
    received_t () = default;
    received_t (const received_t &other) = default;
    received_t &operator= (const received_t &other) = default;

    received_t (received_t &&) noexcept = default;
    received_t &operator= (received_t &&) noexcept = default;

    const std::optional<routing_id_t> &routing_id () const noexcept { return _routing_id; }

    const std::optional<uint64_t> &request_seq () const noexcept { return _request_seq; }

    const std::vector<message_t> &parts () const;
    std::vector<message_t> &parts ();

    bool is_single_part () const noexcept
    {
        return _parts.is_single_part ();
    }
    message_t &first_part ();
    message_t single_part_or_throw ();
    /// Send context (routing_id) is encapsulated. Returns an
    /// operation builder; accumulate payload via `.message(...)`.
    send_operation_t send ();
    /// Reply context (routing_id, request_seq) is encapsulated.
    /// Returns an operation builder; accumulate reply payload via
    /// `.message(...)`. Submit throws if there is no valid reply context.
    reply_operation_t reply ();
    void close ();

  private:
    friend class socket_t;
    friend class router_socket_t;
    friend class send_operation_t;
    friend class send_submit_operation_t;
    friend class reply_operation_t;
    friend class reply_submit_operation_t;
    friend struct detail::received_access_t;

    enum class send_context_kind_t
    {
        none,
        socket_rid
    };

    received_t (std::optional<routing_id_t> routing_id_,
                std::optional<uint64_t> request_seq_,
                std::vector<message_t> parts_);

    received_t (std::optional<routing_id_t> routing_id_,
                std::optional<uint64_t> request_seq_,
                message_t part_);

    std::optional<routing_id_t> _routing_id;
    std::optional<uint64_t> _request_seq;
    detail::lazy_message_parts_t _parts;
    // Send/reply context, reconstructed lazily at submit time from the stored
    // routing ids and request sequence. Avoids per-receive std::function
    // closures and their heap allocations on the server hot path.
    std::uintptr_t _send_context_handle = 0;
    send_context_kind_t _send_context_kind = send_context_kind_t::none;
};

} // namespace zlink
