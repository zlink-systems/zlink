/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_RECEIVED_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_RECEIVED_ACCESS_HPP_INCLUDED

#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>
#include <zlink/Contracts/Messaging/received.hpp>

namespace zlink::detail
{

struct received_access_t
{
    static lazy_message_parts_t &prepare_receive (received_t &received_) noexcept
    {
        received_._routing_id.reset ();
        received_._reply_token.reset ();
        clear_context (received_);
        return received_._parts;
    }

    static reply_token_t make_reply_token (
      const std::shared_ptr<const void> &owner_, uint64_t value_)
    {
        return reply_token_t (owner_, value_);
    }

    static bool token_owner_matches (const reply_token_t &token_,
                                     const std::shared_ptr<const void> &owner_) noexcept
    {
        return token_._owner.get () == owner_.get ();
    }

    static uint64_t token_value (const reply_token_t &token_) noexcept
    {
        return token_._value;
    }

    static void commit_receive_metadata (
      received_t &received_, routing_id_t source_rid_, bool has_reply_token_,
      uint64_t reply_token_value_, const std::shared_ptr<const void> &reply_owner_)
    {
        received_._routing_id = zlink::detail::routing_id_empty (source_rid_)
                                  ? std::nullopt
                                  : std::optional<routing_id_t> (std::move (source_rid_));
        received_._reply_token = has_reply_token_
                                   ? std::optional<reply_token_t> (
                                       make_reply_token (reply_owner_, reply_token_value_))
                                   : std::nullopt;
    }

    static void assign (received_t &received_,
                        std::optional<routing_id_t> routing_id_,
                        std::optional<reply_token_t> reply_token_,
                        std::vector<message_t> &parts_)
    {
        received_._routing_id = std::move (routing_id_);
        received_._reply_token = std::move (reply_token_);
        received_._parts.replace (parts_);
        clear_context (received_);
    }

    static void assign (received_t &received_,
                        std::optional<routing_id_t> routing_id_,
                        std::optional<reply_token_t> reply_token_, message_t part_)
    {
        received_._routing_id = std::move (routing_id_);
        received_._reply_token = std::move (reply_token_);
        received_._parts.replace (std::move (part_));
        clear_context (received_);
    }

    static received_t make (std::optional<routing_id_t> routing_id_,
                            std::optional<reply_token_t> reply_token_,
                            std::vector<message_t> parts_)
    {
        return received_t (std::move (routing_id_), std::move (reply_token_),
                           std::move (parts_));
    }

    static received_t make (std::optional<routing_id_t> routing_id_,
                            std::optional<reply_token_t> reply_token_, message_t part_)
    {
        return received_t (std::move (routing_id_), std::move (reply_token_),
                           std::move (part_));
    }

    static void set_socket_rid_send_context (
      received_t &received_, void *handle_,
      const std::shared_ptr<socket_runtime_state_t> &runtime_)
    {
        received_._send_context_handle = reinterpret_cast<std::uintptr_t> (handle_);
        received_._send_context_kind = received_t::send_context_kind_t::socket_rid;
        received_._send_context_runtime = runtime_;
    }

    static bool has_send_context (const received_t &received_) noexcept
    {
        return received_._send_context_handle != 0
               && received_._send_context_kind == received_t::send_context_kind_t::socket_rid
               && received_._routing_id.has_value ()
               && !received_._send_context_runtime.expired ();
    }

    static bool has_reply_context (const received_t &received_) noexcept
    {
        return has_send_context (received_) && received_._reply_token.has_value ();
    }

    static void *send_handle (const received_t &received_) noexcept
    {
        return reinterpret_cast<void *> (received_._send_context_handle);
    }

    static std::shared_ptr<socket_runtime_state_t> runtime (const received_t &received_)
    {
        return received_._send_context_runtime.lock ();
    }

  private:
    static void clear_context (received_t &received_) noexcept
    {
        received_._send_context_handle = 0;
        received_._send_context_kind = received_t::send_context_kind_t::none;
        received_._send_context_runtime.reset ();
    }
};

} // namespace zlink::detail

#endif
