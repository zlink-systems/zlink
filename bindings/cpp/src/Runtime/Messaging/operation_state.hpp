/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_STATE_HPP_INCLUDED

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Messaging/operation_builder_base.hpp>
#include "../Core/routing_id_access.hpp"

namespace zlink
{

namespace detail
{
enum class operation_kind_t
{
    // Neutral sentinel used by the pooled-state reset; always overwritten
    // by the operation factory before the state is submitted.
    none,
    // Raw-socket transport operations (PAIR/DEALER/ROUTER/PUB/STREAM).
    raw_send,
    raw_routed_send,
    raw_publish,
    raw_request,
    raw_routed_request,
    raw_reply,
    // Built-in received_t.send()/reply() builders. The state borrows a
    // reference to the originating received_t.
    received_send,
    received_reply
};

struct operation_state_t
{
    operation_kind_t kind = operation_kind_t::none;

    struct routing_target_t
    {
        std::optional<routing_id_t> first_rid;
        zlink_routing_id_t first_rid_native_cache{};
        bool has_first_rid_native_cache = false;

        void reset () noexcept
        {
            first_rid.reset ();
            first_rid_native_cache = zlink_routing_id_t{};
            has_first_rid_native_cache = false;
        }
    };

    struct message_parts_t
    {
        std::optional<message_t> single_part;
        message_t *single_part_source = nullptr;
        bool discard_single_part_on_backpressure = false;
        std::vector<message_t> parts;

        void reset () noexcept
        {
            single_part.reset ();
            single_part_source = nullptr;
            discard_single_part_on_backpressure = false;
            parts.clear ();
        }
    };

    struct raw_command_t
    {
        void *socket = nullptr;
        std::string topic;
        routing_target_t target;

        void reset () noexcept
        {
            socket = nullptr;
            topic.clear ();
            target.reset ();
        }
    };

    // Raw ROUTER reply state.
    struct reply_command_t
    {
        uint64_t request_seq = 0;

        void reset () noexcept { request_seq = 0; }
    };

    struct received_command_t
    {
        received_t *received = nullptr;

        void reset () noexcept { received = nullptr; }
    };

    message_parts_t message;
    raw_command_t raw;
    reply_command_t reply;
    received_command_t received;
    send_flags_t flags = send_flags_t::none;
    std::chrono::milliseconds timeout{};
};

inline void cache_first_rid_native (operation_state_t::routing_target_t &target_,
                                    const routing_id_t &rid_) noexcept
{
    target_.first_rid_native_cache = *zlink::detail::routing_id_native (rid_);
    target_.has_first_rid_native_cache = true;
    target_.first_rid.reset ();
}

inline const zlink_routing_id_t *target_first_rid_native (
  const operation_state_t::routing_target_t &target_) noexcept
{
    if (target_.has_first_rid_native_cache)
        return &target_.first_rid_native_cache;
    if (target_.first_rid.has_value ())
        return zlink::detail::routing_id_native (*target_.first_rid);
    return nullptr;
}

inline bool has_send_parts (const operation_state_t &state_) noexcept
{
    return state_.message.single_part.has_value () || state_.message.single_part_source
           || !state_.message.parts.empty ();
}

inline size_t send_part_count (const operation_state_t &state_) noexcept
{
    return state_.message.single_part.has_value () || state_.message.single_part_source
             ? 1u
             : state_.message.parts.size ();
}

inline message_t &send_single_part (operation_state_t &state_) noexcept
{
    if (state_.message.single_part.has_value ())
        return *state_.message.single_part;
    if (state_.message.single_part_source)
        return *state_.message.single_part_source;
    return state_.message.parts.front ();
}

inline void append_send_part (operation_state_t &state_, message_t &part_)
{
    if (state_.message.single_part.has_value ()) {
        state_.message.parts.push_back (std::move (*state_.message.single_part));
        state_.message.single_part.reset ();
        state_.message.single_part_source = nullptr;
    } else if (state_.message.single_part_source) {
        state_.message.parts.push_back (std::move (*state_.message.single_part_source));
        state_.message.single_part_source = nullptr;
    }
    state_.message.parts.push_back (std::move (part_));
}

inline bool can_borrow_single_send_part (operation_kind_t kind_) noexcept
{
    switch (kind_) {
        case operation_kind_t::raw_send:
        case operation_kind_t::raw_routed_send:
        case operation_kind_t::raw_publish:
            return true;
        default:
            return false;
    }
}

inline void restore_single_send_part_to_source (operation_state_t &state_) noexcept
{
    if (!state_.message.single_part_source || !state_.message.single_part.has_value ()
        || !state_.message.single_part->valid ())
        return;
    *state_.message.single_part_source = std::move (*state_.message.single_part);
    state_.message.single_part_source = nullptr;
}

inline void restore_single_send_part_to_source (operation_state_t &state_,
                                                std::vector<message_t> &parts_) noexcept
{
    if (!state_.message.single_part_source || parts_.size () != 1u || !parts_[0].valid ())
        return;
    *state_.message.single_part_source = std::move (parts_[0]);
    state_.message.single_part_source = nullptr;
}

inline void restore_send_parts_to_state (operation_state_t &state_,
                                         std::vector<message_t> &parts_) noexcept
{
    const bool had_single_part_source = state_.message.single_part_source != nullptr;
    restore_single_send_part_to_source (state_, parts_);
    if (had_single_part_source || parts_.empty ())
        return;
    if (parts_.size () == 1u && state_.message.single_part.has_value ()) {
        state_.message.single_part = std::move (parts_[0]);
        return;
    }
    state_.message.parts = std::move (parts_);
}

// Thread-local pool of operation_state_t to avoid per-send heap alloc.
// Each send/request/reply chain acquires one pooled state at the entry factory
// and returns it on submit success. The pool keeps string/vector capacity so
// repeated PAIR/DEALER/PUBSUB sends with empty topic or short topic do not
// trigger malloc/free per call.
inline void reset_for_reuse (operation_state_t &state_) noexcept
{
    // RAW_SEND_HOT_PATH: pair/dealer send only populates the message, raw
    // socket, and flags fields. Every other operation takes the complete
    // reset below before the pooled state becomes reusable.
    if (state_.kind == operation_kind_t::raw_send) {
        state_.kind = operation_kind_t::none;
        state_.message.reset ();
        state_.raw.socket = nullptr;
        state_.flags = send_flags_t::none;
        return;
    }

    state_.kind = operation_kind_t::none;
    state_.message.reset ();
    state_.raw.reset ();
    state_.reply.reset ();
    state_.received.reset ();
    state_.flags = send_flags_t::none;
    state_.timeout = std::chrono::milliseconds{};
}

inline std::vector<std::unique_ptr<operation_state_t>> &state_pool () noexcept
{
    static thread_local std::vector<std::unique_ptr<operation_state_t>> pool;
    return pool;
}

inline std::unique_ptr<operation_state_t> acquire_state ()
{
    auto &pool = state_pool ();
    if (!pool.empty ()) {
        auto state = std::move (pool.back ());
        pool.pop_back ();
        return state;
    }
    return std::make_unique<operation_state_t> ();
}

inline void release_state (std::unique_ptr<operation_state_t> state_ptr_) noexcept
{
    if (!state_ptr_)
        return;
    constexpr size_t k_pool_cap = 8;
    auto &pool = state_pool ();
    if (pool.size () >= k_pool_cap)
        return;
    reset_for_reuse (*state_ptr_);
    pool.push_back (std::move (state_ptr_));
}

inline void pooled_operation_state_policy_t::destroy (
  std::unique_ptr<operation_state_t> state_ptr_) noexcept
{
    release_state (std::move (state_ptr_));
}

} // namespace detail


} // namespace zlink

#endif
