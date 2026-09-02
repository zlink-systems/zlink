/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"
#include "../Sockets/results.hpp"
#include "lazy_message_parts.hpp"
#include "message.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace zlink
{

namespace detail
{
struct received_access_t;
struct socket_runtime_state_t;
}

class reply_token_t final
{
  public:
    reply_token_t () = delete;
    reply_token_t (const reply_token_t &) = default;
    reply_token_t &operator= (const reply_token_t &) = default;

    friend bool operator== (const reply_token_t &left_,
                            const reply_token_t &right_) noexcept
    {
        return left_._owner.get () == right_._owner.get ()
               && left_._value == right_._value;
    }

  private:
    reply_token_t (std::shared_ptr<const void> owner_, uint64_t value_) noexcept :
        _owner (std::move (owner_)), _value (value_)
    {
    }

    std::shared_ptr<const void> _owner;
    uint64_t _value;

    friend struct detail::received_access_t;
    friend struct reply_token_hash_t;
};

inline bool operator!= (const reply_token_t &left_,
                        const reply_token_t &right_) noexcept
{
    return !(left_ == right_);
}

struct reply_token_hash_t
{
    std::size_t operator() (const reply_token_t &token_) const noexcept
    {
        const std::size_t owner_hash =
          std::hash<const void *> {} (token_._owner.get ());
        const std::size_t value_hash = std::hash<uint64_t> {} (token_._value);
        return owner_hash ^ (value_hash + 0x9e3779b9u + (owner_hash << 6)
                             + (owner_hash >> 2));
    }
};

/// @brief A received message envelope: routing metadata, parts, and an optional reply context.
class received_t
{
  public:
    received_t () = default;
    ~received_t ();
    received_t (const received_t &other) = default;
    received_t &operator= (const received_t &other) = default;

    received_t (received_t &&) noexcept = default;
    received_t &operator= (received_t &&) noexcept = default;

    const std::optional<routing_id_t> &routing_id () const noexcept { return _routing_id; }

    const std::optional<reply_token_t> &reply_token () const noexcept
    {
        return _reply_token;
    }

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
    /// Reply context (routing_id and opaque token) is encapsulated.
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
                std::optional<reply_token_t> reply_token_,
                std::vector<message_t> parts_);

    received_t (std::optional<routing_id_t> routing_id_,
                std::optional<reply_token_t> reply_token_,
                message_t part_);

    std::optional<routing_id_t> _routing_id;
    std::optional<reply_token_t> _reply_token;
    detail::lazy_message_parts_t _parts;
    // Send/reply context captured from the receiving socket. The runtime tag
    // keeps completion ownership and reply-token validation socket-local.
    std::uintptr_t _send_context_handle = 0;
    send_context_kind_t _send_context_kind = send_context_kind_t::none;
    std::weak_ptr<detail::socket_runtime_state_t> _send_context_runtime;
};

} // namespace zlink
