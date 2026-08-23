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
#include "../Sockets/socket_callback_state.hpp"

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
            // The native cache is only readable while its presence flag is
            // set, so clearing the flag is the whole reset. Zeroing the
            // 256-byte payload would be dead work on every state reuse.
            has_first_rid_native_cache = false;
        }
    };

    struct message_parts_t
    {
        std::optional<message_t> single_part;
        message_t *single_part_source = nullptr;
        std::vector<message_t> parts;
        // Parallel to `parts`: for every part added from an lvalue message_t,
        // the caller object it was moved out of; nullptr for parts added from
        // an rvalue (those are consumed on submit and have no owner to return
        // to). The vector is kept the same length as `parts` so a failed
        // submit can hand every caller-owned part back, which is the multipart
        // form of the single-part `single_part_source` restore.
        std::vector<message_t *> part_sources;

        void reset () noexcept
        {
            single_part.reset ();
            single_part_source = nullptr;
            parts.clear ();
            part_sources.clear ();
        }
    };

    struct raw_command_t
    {
        void *socket = nullptr;
        // Non-owning view of the callback state owned by the originating
        // socket_t. Reading it is only defined while `callbacks_anchor` has
        // not expired; `live_callback_state()` enforces that.
        socket_callback_state_t *callbacks = nullptr;
        // Lifetime token for `callbacks`, and the only owner-tracking cost the
        // operation state pays. It is deliberately kept across pooled reuse so
        // a hot send loop on one socket rebinds it at most once.
        std::weak_ptr<socket_callback_state_t> callbacks_anchor;
        std::string topic;
        routing_target_t target;

        void reset () noexcept
        {
            socket = nullptr;
            // `callbacks`/`callbacks_anchor` are a socket-keyed cache, not
            // per-operation state: bind_callback_state() revalidates them.
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

// Lifetime ownership of the callback state belongs to socket_t. An operation
// state only needs to be able to tell whether that owner is still alive, so it
// carries one weak token plus the raw view. The token is bound once per
// (pooled state, socket) pair instead of once per call.
inline void bind_callback_state (operation_state_t::raw_command_t &raw_,
                                 socket_callback_state_t &state_)
{
    if (raw_.callbacks == &state_ && !raw_.callbacks_anchor.expired ())
        return;
    raw_.callbacks = &state_;
    raw_.callbacks_anchor = state_.weak_from_this ();
}

// Synchronous terminals: the submitting statement cannot outlive the socket_t
// that owns the callback state, so no strong reference is taken. Returns
// nullptr when the owning socket is already gone.
inline socket_callback_state_t *live_callback_state (
  const operation_state_t::raw_command_t &raw_) noexcept
{
    if (!raw_.callbacks || raw_.callbacks_anchor.expired ())
        return nullptr;
    return raw_.callbacks;
}

// Terminals whose state outlives the submitting statement must own a strong
// reference to the callback state instead of the weak view above.
inline std::shared_ptr<socket_callback_state_t> share_callback_state (
  const operation_state_t::raw_command_t &raw_)
{
    return raw_.callbacks_anchor.lock ();
}

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

//  Appends one part that the state does not own to the multipart sequence and
//  remembers @p source_ as the object a failed submit must give it back to.
//  Passing nullptr marks the part as rvalue-owned (consumed on submit).
inline void append_send_part_from (operation_state_t &state_,
                                   message_t &part_,
                                   message_t *source_)
{
    state_.message.parts.push_back (std::move (part_));
    state_.message.part_sources.push_back (source_);
}

//  Moves the part staged in the single-part fast path (if any) into the
//  multipart sequence, keeping its caller-source association.
inline void fold_staged_single_part (operation_state_t &state_)
{
    if (state_.message.single_part.has_value ()) {
        // A staged single_part that also has a source is a caller lvalue; one
        // without a source came from an rvalue and stays consumed on submit.
        append_send_part_from (state_, *state_.message.single_part,
                               state_.message.single_part_source);
        state_.message.single_part.reset ();
        state_.message.single_part_source = nullptr;
    } else if (state_.message.single_part_source) {
        append_send_part_from (state_, *state_.message.single_part_source,
                               state_.message.single_part_source);
        state_.message.single_part_source = nullptr;
    }
}

inline void append_send_part (operation_state_t &state_, message_t &part_)
{
    fold_staged_single_part (state_);
    append_send_part_from (state_, part_, &part_);
}

//  Rvalue parts are consumed on submit, so they carry no restore source. Only
//  the submit-stage builders call this, and those are only reachable after a
//  first part was staged, so folding always yields a real multipart sequence.
inline void append_send_part (operation_state_t &state_, message_t &&part_)
{
    fold_staged_single_part (state_);
    append_send_part_from (state_, part_, nullptr);
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

//  Failure-path counterpart of `append_send_part()`: hands every caller-owned
//  part in @p parts_ back to the message_t it was taken from, which is the
//  documented "on failure ownership returns to the caller" rule applied to a
//  multipart sequence. Parts added as rvalues carry no source and follow the
//  consumed-on-submit rule, so they are left in @p parts_ to be destroyed.
//  Parts the transport already consumed are invalid here and are skipped, so a
//  partial submit failure restores exactly the parts the caller still owns.
inline void restore_send_parts_to_sources (operation_state_t &state_,
                                           std::vector<message_t> &parts_) noexcept
{
    if (state_.message.single_part_source) {
        restore_single_send_part_to_source (state_, parts_);
        return;
    }
    const size_t count = parts_.size () < state_.message.part_sources.size ()
                           ? parts_.size ()
                           : state_.message.part_sources.size ();
    for (size_t i = 0; i < count; ++i) {
        message_t *const source = state_.message.part_sources[i];
        if (!source || source == &parts_[i] || !parts_[i].valid ())
            continue;
        *source = std::move (parts_[i]);
    }
    state_.message.part_sources.clear ();
}

inline void restore_send_parts_to_state (operation_state_t &state_,
                                         std::vector<message_t> &parts_) noexcept
{
    const bool had_single_part_source = state_.message.single_part_source != nullptr;
    restore_send_parts_to_sources (state_, parts_);
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
